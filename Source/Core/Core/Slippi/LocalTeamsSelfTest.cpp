#include "Core/Slippi/LocalTeamsSelfTest.h"
#include "Core/Slippi/LocalTeamsCoordinator.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/Slippi/SlippiMatchmaking.h"
#include "SlippiRustExtensions.h"
#include "Core/ConfigManager.h"
#include "Common/Thread.h"
#include <chrono>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace
{
void Require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}
void Await(const std::function<bool()>& predicate, const char* message)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!predicate())
  {
    Require(std::chrono::steady_clock::now() < deadline, message);
    Common::SleepCurrentThread(2);
  }
}

// Exercise cancellation while real matchmaking workers wait on a silent local UDP socket.
// No authentication or production matchmaking request is sent.
class CancelMatchmakingFixture final : public SlippiMatchmaking
{
public:
  CancelMatchmakingFixture() : SlippiMatchmaking(0, nullptr, 49124) {}
  std::atomic<bool> waiting{false};
  void WaitOn(const ENetAddress& address)
  {
    m_client = enet_host_create(nullptr, 1, 1, 0, 0);
    Require(m_client != nullptr, "cancel fixture could not create an ENet host");
    m_matchmakeThread = std::thread([this, address] {
      waiting.store(true, std::memory_order_release);
      SlippiStunObservation result{};
      stunBindingRequest(address, result);
    });
  }
};
}

#include "Core/Slippi/LocalTeamsAsmSelfTest.h"

int RunLocalTeamsSelfTest(const std::string& reportPath)
{
  std::ofstream report(reportPath);
  if (!report) return 2;
  report << std::unitbuf; // Keep diagnostics if a headless fixture fails or stalls.
  if (enet_initialize() != 0) { report << "FAIL: ENet initialization\n"; return 1; }
  int result = 0;
  try
  {
    const auto separator = reportPath.find_last_of("/\\");
    LocalTeamsAsmTest::Run(separator == std::string::npos ? "" : reportPath.substr(0, separator + 1), report);
    constexpr u16 basePort = 49120;
    // Fail before a constructor can show a GUI error if a test port is occupied.
    std::array<ENetHost*, 4> probes{};
    bool available = true;
    for (unsigned i = 0; i < 4; ++i)
    {
      ENetAddress address{ENET_HOST_ANY, u16(basePort + i)};
      probes[i] = enet_host_create(&address, 10, 3, 0, 0);
      available &= probes[i] != nullptr;
    }
    for (auto* host : probes) if (host) enet_host_destroy(host);
    Require(available, "localhost ports 49120-49123 unavailable");
    SConfig::GetInstance().bQoSEnabled = false;
    std::array<std::unique_ptr<SlippiMatchmaking>, 4> matchmaking;
    std::array<std::unique_ptr<SlippiNetplayClient>, 4> clients;
    for (u8 i = 0; i < 4; ++i)
    {
      matchmaking[i] = std::make_unique<SlippiMatchmaking>(0, nullptr);
      matchmaking[i]->SetupLocalTeamsLoopback(i, basePort);
    }
    Await([&] {
      for (auto& mm : matchmaking)
        if (mm->GetMatchmakeState() != SlippiMatchmaking::CONNECTION_SUCCESS) return false;
      return true;
    }, "four localhost clients did not connect");
    for (unsigned i = 0; i < 4; ++i) clients[i] = matchmaking[i]->GetNetplayClient();
    Require(SLIPPI_NETPLAY_CLIENT_COUNT.load() == 4 && IsOnline(), "four independent client lifetimes");
    report << "PASS: four real SlippiNetplayClient objects, 12 peer links, one process\n";

    auto select = [&] {
      for (u8 i = 0; i < 4; ++i)
      {
        SlippiPlayerSelections selection;
        selection.characterId = i + 1; selection.characterColor = i;
        selection.teamId = i / 2; selection.rngOffset = 100 + i;
        selection.isCharacterSelected = selection.isStageSelected = true; selection.stageId = 0x1F;
        clients[i]->SetMatchSelections(selection);
      }
      Await([&] {
        for (const auto& client : clients)
        {
          const auto info = client->GetMatchInfoSnapshot();
          if (!info.localPlayerSelections.isCharacterSelected) return false;
          for (const auto& player : info.remotePlayerSelections)
            if (!player.isCharacterSelected || player.characterId != player.playerIdx + 1 || player.teamId != player.playerIdx / 2)
              return false;
        }
        return true;
      }, "selection propagation to every peer");
    };
    auto play = [&](s32 frameCount) {
      for (auto& client : clients) client->StartSlippiGame();
      for (s32 frame = 1; frame <= frameCount; ++frame)
      {
        for (u8 i = 0; i < 4; ++i)
        {
          u8 pad[12] = {0, u8(1u << i), u8(frame % 80), i, 0, 0, 0, 0, 0, 0, 0, 0};
          clients[i]->SendSlippiPad(std::make_unique<SlippiPad>(frame, frame - 1, 0x12000000u + frame, pad));
        }
        Await([&] {
          for (const auto& client : clients)
            for (unsigned peer = 0; peer < 3; ++peer)
              if (client->GetSlippiRemotePad(peer, 7)->latestFrame < frame) return false;
          return true;
        }, "input delivery to every localhost peer");
        for (const auto& client : clients)
          for (unsigned peer = 0; peer < 3; ++peer)
          {
            const auto pad = client->GetSlippiRemotePad(peer, 7);
            Require(pad->data.size() >= 8 && pad->data[1] == (1u << pad->playerIdx) && pad->data[2] == frame % 80,
                    "physical stream arrived at wrong logical slot");
            Require(pad->checksumFrame == frame - 1 && pad->checksum == 0x12000000u + frame, "canonical checksum routing");
          }
        for (const auto& client : clients)
        {
          client->DropOldRemoteInputs(frame - 2);
          if (frame % 30 == 0) client->SendSlippiPad(nullptr); // stall/resend path
        }
      }
    };
    select(); report << "PASS: distinct characters, costumes, teams and RNG selections propagate\n";
    play(360); report << "PASS: 360 frames on all four streams; checksums, receive pruning and resend\n";
    select(); play(60); report << "PASS: rematch resets and another 60 frames\n";
    clients[3].reset();
    Require(IsOnline() && SLIPPI_NETPLAY_CLIENT_COUNT.load() == 3, "secondary destruction cleared primary online state");
    report << "PASS: destroying secondary preserves primary online state\n";
    for (auto& client : clients) client.reset();
    Require(!IsOnline() && SLIPPI_NETPLAY_CLIENT_COUNT.load() == 0, "client cleanup leaked online state");
    report << "PASS: all network clients shut down cleanly\n";

    // Exercise the actual EXI coordinator, including physical-port mapping and frame gating.
    // The caller uses a disposable --user directory; no real account or configuration is read.
    File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount=4\nLoopbackTest=True\nController1=1\nController2=3\nController3=2\nController4=4\nBasePort=49120\n",
                            File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini");
    LocalTeamsCoordinator coordinator(0, nullptr, [] { return u16(0x8); });
    std::unique_ptr<SlippiMatchmaking> primaryMm = std::make_unique<SlippiMatchmaking>(0, nullptr);
    std::unique_ptr<SlippiNetplayClient> primary;
    const u8 buttons[8] = {};
    auto confirmAll = [&] {
      for (u8 i = 0; i < 4; ++i)
      {
        const auto status = coordinator.Poll(SlippiMatchmaking::TEAMS, buttons);
        Require(status[0] && status[2] == i && status[3] == coordinator.state.ports[i], "picker controller handoff");
        const u8 selection[9] = {u8(i / 2), u8(i + 1), i, 1, 0, 0x1F, 1, 3, 0};
        Require(coordinator.Confirm(selection, 100 + i), "coordinator selection confirmation");
        if (i < 3) Require(!coordinator.BeginSearch(primaryMm), "early Start began matchmaking");
      }
    };
    confirmAll();
    Require(coordinator.BeginSearch(primaryMm), "coordinator loopback group startup");
    Await([&] { return coordinator.Tick(*primaryMm, nullptr) == SlippiMatchmaking::CONNECTION_SUCCESS; }, "coordinator assignment gate");
    primary = primaryMm->GetNetplayClient();
    coordinator.PublishSelections(*primary);
    Await([&] { return coordinator.AllReady(*primary); }, "coordinator aggregate ready gate");
    Require(coordinator.state.AllConfirmed(), "local ready gate");
    report << "PASS: actual coordinator sequential selection, lobby validation and aggregate readiness\n";
    auto sendGame = [&](s32 frameCount) {
      primary->StartSlippiGame();
      coordinator.StartGame();
      for (s32 frame = 1; frame <= frameCount; ++frame)
      {
        u8 raw[48] = {};
        for (u8 port = 0; port < 4; ++port)
        { raw[port * 12 + 1] = 1u << port; raw[port * 12 + 2] = u8(50 + frame % 20); }
        if (frame == 1)
          for (s32 neutral = 1; neutral <= 2; ++neutral) primary->SendSlippiPad(std::make_unique<SlippiPad>(neutral));
        coordinator.SendInputs(frame, 2, frame - 1, 0x34000000u + frame, raw, true);
        Require(coordinator.state.lastSentFrame == frame - 1, "stall created a new guest sample");
        primary->SendSlippiPad(std::make_unique<SlippiPad>(frame + 2, frame - 1, 0x34000000u + frame, raw));
        coordinator.SendInputs(frame, 2, frame - 1, 0x34000000u + frame, raw, false);
        u8 poison[48] = {};
        coordinator.SendInputs(frame, 2, frame - 1, 0xDEADBEEF, poison, false);
        Await([&] {
          for (unsigned peer = 0; peer < 3; ++peer)
            if (primary->GetSlippiRemotePad(peer, 7)->latestFrame < frame + 2) return false;
          return true;
        }, "coordinator guest input publication");
        for (unsigned peer = 0; peer < 3; ++peer)
        {
          const auto pad = primary->GetSlippiRemotePad(peer, 7);
          const auto physical = coordinator.state.ports[pad->playerIdx];
          Require(pad->data[1] == (1u << physical) && pad->data[2] == 50 + frame % 20, "coordinator physical pad routing");
          Require(pad->checksum == 0x34000000u + frame, "duplicate/rollback sample overwrote original checksum");
        }
        primary->DropOldRemoteInputs(frame - 1);
      }
    };
    sendGame(120);
    confirmAll();
    Require(SLIPPI_NETPLAY_CLIENT_COUNT.load() == 4, "rematch recreated network clients");
    coordinator.Tick(*primaryMm, primary.get());
    Await([&] { return coordinator.AllReady(*primary); }, "coordinator rematch ready gate");
    sendGame(60);
    primary->ForceDisconnectPlayer(1);
    Require(coordinator.Tick(*primaryMm,primary.get())==SlippiMatchmaking::ERROR_ENCOUNTERED &&
            !coordinator.LobbyDisconnected() && coordinator.Error().find("Local P1")!=std::string::npos,
            "loss of another local session was treated as a normal remote lobby exit");
    report << "PASS: losing an assigned local peer reports the affected local connection\n";
    coordinator.Cleanup();
    Require(IsOnline() && SLIPPI_NETPLAY_CLIENT_COUNT.load() == 1, "coordinator cleanup affected primary lifetime");
    primary.reset(); primaryMm.reset();
    Require(!IsOnline(), "coordinator left a network client running");
    report << "PASS: coordinator mapped controllers 1/3/2/4, common delay/checksums, stalls, duplicate rejection, rematch and cleanup\n";
    // Two native local groups, including guests in the lowest network slots.
    // Supply a deterministic stage chooser to check the proposal that actually
    // reaches all four ENet clients, independent of Start order and polling.
    {
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount=2\nBasePort=49120\n",
                              File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini");
      const std::array<u16, 5> stages{{0x3, 0x8, 0x1C, 0x1F, 0x20}};
      std::array<unsigned, 2> draws{};
      std::array<bool, 2> connected{};
      std::array<std::unique_ptr<LocalTeamsCoordinator>, 2> teams;
      std::array<std::unique_ptr<SlippiMatchmaking>, 2> teamMm;
      std::array<std::unique_ptr<SlippiNetplayClient>, 2> teamPrimary;
      for (unsigned side = 0; side < 2; ++side)
        teams[side] = std::make_unique<LocalTeamsCoordinator>(0, nullptr, [&, side] {
          ++draws[side];
          // The service list excludes this provisional pre-connection stage.
          return connected[side] ? stages[(draws[side] - 1 + 2 * side) % stages.size()] : u16(0x2);
        });
      std::array<u16, 10> chosen{};
      for (unsigned game = 0; game < chosen.size(); ++game)
      {
        for (unsigned side = 0; side < 2; ++side)
        {
          auto& team = *teams[side];
          team.Poll(SlippiMatchmaking::TEAMS, buttons); // release/arm both Starts
          const unsigned before = draws[side];
          for (unsigned pick = 0; pick < 2; ++pick)
          {
            const unsigned player = (pick + game + side) % 2;
            const u8 selection[9] = {u8(side), u8(1 + 2 * side + player), 0, 1, 0, 0, 3, u8(0x80 | player), 0};
            Require(team.Confirm(selection, 500 + game * 4 + side * 2 + player), "two-team native confirmation");
            Require(draws[side] == before + pick, "random stage was drawn before the whole local group was ready");
          }
          if (game == 0)
          {
            teamMm[side] = std::make_unique<SlippiMatchmaking>(0, nullptr);
            teamMm[side]->SetupLocalTeamsLoopback(u8(2 + side), basePort);
            team.guests[0].matchmaking = std::make_unique<SlippiMatchmaking>(0, nullptr);
            team.guests[0].matchmaking->SetupLocalTeamsLoopback(u8(side), basePort);
          }
        }
        if (game == 0)
        {
          Await([&] {
            bool ready = true;
            for (unsigned side = 0; side < 2; ++side)
              ready &= teams[side]->Tick(*teamMm[side], nullptr) == SlippiMatchmaking::CONNECTION_SUCCESS;
            return ready;
          }, "two local teams did not connect");
          for (unsigned side = 0; side < 2; ++side)
          {
            teamPrimary[side] = teamMm[side]->GetNetplayClient();
            connected[side] = true;
            teams[side]->RefreshStage(); // use the newly received service list
          }
        }
        std::array<u16, 2> proposals{};
        for (unsigned side = 0; side < 2; ++side)
        {
          proposals[side] = teams[side]->PrimarySelection().stageId;
          Require(std::find(stages.begin(), stages.end(), proposals[side]) != stages.end(),
                  "local group retained a stage outside the service list");
          teams[side]->PublishSelections(*teamPrimary[side]);
        }
        Await([&] { return teams[0]->AllReady(*teamPrimary[0]) && teams[1]->AllReady(*teamPrimary[1]); },
              "two-team rematch did not become ready");
        for (unsigned side = 0; side < 2; ++side)
        {
          auto& team = *teams[side];
          for (auto* client : {teamPrimary[side].get(), team.guests[0].netplay.get()})
          {
            const auto info = client->GetMatchInfoSnapshot();
            u16 selected = 0;
            auto check = [&](const SlippiPlayerSelections& selection) {
              Require(selection.isStageSelected && selection.stageId == proposals[selection.playerIdx % 2],
                      "primary or guest replaced the group's random stage proposal");
              if (selection.playerIdx == 0) selected = selection.stageId;
            };
            check(info.localPlayerSelections);
            for (const auto& selection : info.remotePlayerSelections) check(selection);
            Require(selected == proposals[0], "four peers disagreed about the lowest-slot stage choice");
          }
          team.PublishSelections(*teamPrimary[side]);
          team.Tick(*teamMm[side], teamPrimary[side].get());
          Require(draws[side] == game + 2 && team.PrimarySelection().stageId == proposals[side],
                  "readiness polling redrew the stage or rematch reused an old proposal");
        }
        chosen[game] = proposals[0];
        if (game % stages.size())
          Require(std::find(chosen.begin() + game - game % stages.size(), chosen.begin() + game, chosen[game]) == chosen.begin() + game,
                  "local group lost the chooser's stage rotation");
        for (unsigned side = 0; side < 2; ++side)
        { teamPrimary[side]->StartSlippiGame(); teams[side]->StartGame(); }
      }
      for (auto& team : teams) team->Cleanup();
      for (auto& client : teamPrimary) client.reset();
      for (auto& mm : teamMm) mm.reset();
      Require(!IsOnline(), "two-team stage fixture leaked a network client");
      report << "PASS: two native local teams rotate stages over ten games; both Start orders, guest stage authority, service-list refresh and stable four-peer agreement\n";
    }
    // Three local sessions and an independent fourth peer. The transport is
    // localhost here; normal ThreeLocal mode still uses real matchmaking.
    {
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount=3\nLoopbackTest=False\nController1=1\nController2=3\nController3=2\nBasePort=49120\n",
                              File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini");
      LocalTeamsCoordinator trio(0, nullptr, [] { return u16(0x8); });
      auto trioMm = std::make_unique<SlippiMatchmaking>(0, nullptr);
      auto friendMm = std::make_unique<SlippiMatchmaking>(0, nullptr);
      auto pickThree = [&] {
        for (u8 i = 0; i < 3; ++i)
        {
          const auto status = trio.Poll(SlippiMatchmaking::TEAMS, buttons);
          Require(status[0] && status[1] == 3 && status[2] == i && status[3] == trio.state.ports[i], "three-local controller handoff");
          const u8 selection[9] = {u8(i / 2), u8(i + 1), i, 1, 0, 0x1F, 1, 3, 0};
          Require(trio.Confirm(selection, 200 + i), "three-local selection confirmation");
          Require(trio.state.AllConfirmed() == (i == 2), "three-local confirmation gate");
        }
      };
      pickThree();
      Require(trio.state.phase == LocalTeams::Phase::EnteringCode && !IsOnline(), "third pick did not wait for room entry");
      // Deliberately scramble assigned Melee slots independently of hardware:
      // local 1/2/3 -> slots 2/1/4, independent friend -> slot 3.
      trioMm->SetupLocalTeamsLoopback(1, basePort);
      const u8 guestSlots[2] = {0, 3};
      for (unsigned i = 0; i < 2; ++i)
      {
        trio.guests[i].matchmaking = std::make_unique<SlippiMatchmaking>(0, nullptr);
        trio.guests[i].matchmaking->SetupLocalTeamsLoopback(guestSlots[i], basePort);
      }
      friendMm->SetupLocalTeamsLoopback(2, basePort);
      Await([&] {
        return friendMm->GetMatchmakeState() == SlippiMatchmaking::CONNECTION_SUCCESS &&
               trio.Tick(*trioMm, nullptr) == SlippiMatchmaking::CONNECTION_SUCCESS;
      }, "three-local plus independent friend did not connect");
      auto trioPrimary = trioMm->GetNetplayClient();
      auto friendClient = friendMm->GetNetplayClient();
      Require(SLIPPI_NETPLAY_CLIENT_COUNT.load() == 4 && !trio.guests[2].netplay && !trio.guests[2].matchmaking,
              "three-local coordinator created an extra local session");
      auto ready = [&] {
        trio.PublishSelections(*trioPrimary);
        Await([&] {
          const auto info = friendClient->GetMatchInfoSnapshot();
          for (const auto& selection : info.remotePlayerSelections)
            if (!selection.isCharacterSelected) return false;
          return true;
        }, "three local selections did not reach independent friend");
        Require(!trio.AllReady(*trioPrimary), "three-local game launched before friend confirmed");
        SlippiPlayerSelections selection;
        selection.characterId = 20; selection.characterColor = 1; selection.teamId = 1;
        selection.rngOffset = 400; selection.stageId = 0x1F;
        selection.isCharacterSelected = selection.isStageSelected = true;
        friendClient->SetMatchSelections(selection);
        Await([&] { return trio.AllReady(*trioPrimary); }, "three-local readiness did not accept independent friend");
      };
      ready();
      report << "PASS: three local sessions plus independent fourth peer; third Start waits for code, readiness waits for friend\n";
      auto mixedGame = [&](s32 frameCount) {
        trioPrimary->StartSlippiGame(); trio.StartGame(); friendClient->StartSlippiGame();
        const unsigned physicalBySlot[4] = {2, 0, 4, 1};
        for (s32 frame = 1; frame <= frameCount; ++frame)
        {
          u8 raw[48] = {};
          for (u8 port = 0; port < 4; ++port)
          { raw[12 * port + 1] = 1u << port; raw[12 * port + 2] = u8(50 + port + frame % 20); }
          u8 friendPad[12] = {0, 0x80, u8(90 + frame % 20)};
          if (frame == 1)
            for (s32 initial = 1; initial <= 2; ++initial)
            {
              trioPrimary->SendSlippiPad(std::make_unique<SlippiPad>(initial));
              friendClient->SendSlippiPad(std::make_unique<SlippiPad>(initial));
            }
          trioPrimary->SendSlippiPad(std::make_unique<SlippiPad>(frame + 2, frame - 1, 0x56000000u + frame, raw));
          trio.SendInputs(frame, 2, frame - 1, 0x56000000u + frame, raw, false);
          friendClient->SendSlippiPad(std::make_unique<SlippiPad>(frame + 2, frame - 1, 0x56000000u + frame, friendPad));
          unsigned pendingPolls = 0;
          try
          {
            Await([&] {
              bool delivered = true;
              for (unsigned peer = 0; peer < 3; ++peer)
                delivered &= trioPrimary->GetSlippiRemotePad(peer, 7)->latestFrame >= frame + 2 &&
                             friendClient->GetSlippiRemotePad(peer, 7)->latestFrame >= frame + 2;
              if (!delivered && ++pendingPolls % 8 == 0)
              {
                // Melee resends its saved input while waiting for an unsequenced
                // UDP packet. Exercise that same path without repolling pads.
                trioPrimary->SendSlippiPad(nullptr);
                trio.SendInputs(frame, 2, frame - 1, 0x56000000u + frame, raw, true);
                friendClient->SendSlippiPad(nullptr);
              }
              return delivered;
            }, "three-local/friend inputs did not reach all slots");
          }
          catch (...)
          {
            report << "Mixed input timeout at frame " << frame << " (game length " << frameCount << ")";
            for (unsigned peer = 0; peer < 3; ++peer)
              report << " | peer " << peer << " primary=" << trioPrimary->GetSlippiRemotePad(peer, 7)->latestFrame
                     << " friend=" << friendClient->GetSlippiRemotePad(peer, 7)->latestFrame;
            report << '\n';
            throw;
          }
          for (auto* client : {trioPrimary.get(), friendClient.get()})
            for (unsigned peer = 0; peer < 3; ++peer)
            {
              const auto pad = client->GetSlippiRemotePad(peer, 7);
              const bool fromFriend = pad->playerIdx == 2;
              const unsigned physical = physicalBySlot[pad->playerIdx];
              Require(pad->data[1] == (fromFriend ? 0x80 : (1u << physical)) &&
                      pad->data[2] == (fromFriend ? friendPad[2] : raw[physical * 12 + 2]), "three-local/friend physical stream routing");
              Require(pad->checksumFrame == frame - 1 && pad->checksum == 0x56000000u + frame, "three-local/friend common checksum and delay");
            }
          trioPrimary->DropOldRemoteInputs(frame - 1); friendClient->DropOldRemoteInputs(frame - 1);
        }
      };
      mixedGame(120);
      pickThree(); ready(); mixedGame(60);
      // A remote friend uses the ordinary acknowledged departure path. The
      // fast local cancellation path intentionally sends unsequenced UDP and
      // is not a reliable way to model a remote exit in this fixture.
      friendClient->ForceDisconnect();
      Await([&] {
        return trioPrimary->GetActivePlayerIndices().count(2)==0 &&
               trio.guests[0].netplay->GetActivePlayerIndices().count(2)==0;
      }, "remote departure did not reach the local sessions");
      Require(trio.Tick(*trioMm,trioPrimary.get())==SlippiMatchmaking::IDLE && trio.LobbyDisconnected() &&
              trio.Error().empty(),"remote participant exit was misreported as a local peer error");
      report << "PASS: real remote ENet departure after rematch closes the lobby without a local-peer error\n";
      trio.Cleanup();
      Require(SLIPPI_NETPLAY_CLIENT_COUNT.load() == 2, "three-local cleanup affected primary or independent friend");
      trioPrimary.reset(); trioMm.reset(); friendClient.reset(); friendMm.reset();
      Require(!IsOnline(), "three-local test leaked a client");
      report << "PASS: three-local/friend 120 frames and 60-frame rematch, scrambled slots/controllers, unused port ignored and independent cleanup\n";
    }
    {
      ENetAddress address{ENET_HOST_ANY,0};
      std::unique_ptr<ENetHost,decltype(&enet_host_destroy)> server(enet_host_create(&address,4,1,0,0),&enet_host_destroy);
      Require(server!=nullptr,"cancel fixture server could not bind");
      address.host=0x0100007F; address.port=server->address.port;
      std::array<std::unique_ptr<CancelMatchmakingFixture>,4> waiting;
      for(auto& worker : waiting)
      { worker=std::make_unique<CancelMatchmakingFixture>(); worker->WaitOn(address); }
      Await([&]{for(const auto& worker:waiting) if(!worker->waiting.load(std::memory_order_acquire))return false; return true;},
            "cancel fixture workers did not begin receiving");
      Common::SleepCurrentThread(20); // Ensure the receive waits have started.
      const auto started=std::chrono::steady_clock::now();
      for(auto& worker:waiting) worker->RequestStop();
      for(auto& worker:waiting) worker.reset();
      const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
      Require(elapsed<500,"local search cancellation waited for serial receive/disconnect timeouts");
      report << "PASS: four waiting ENet matchmaking clients cancel and join in " << elapsed << " ms, without serial timeouts\n";
    }
    // An empty configured room must still enable the normal UI-driven test.
    for (unsigned count : {1u, 2u, 3u, 4u})
    {
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount=" + std::to_string(count) + "\nLoopbackTest=False\nBasePort=49120\n",
                              File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini");
      LocalTeamsCoordinator lobby(0, nullptr, [] { return u16(0x8); });
      for (u8 i = 0; i < count; ++i)
      {
        const auto status = lobby.Poll(SlippiMatchmaking::TEAMS, buttons);
        Require(status[0] && !status[8], "normal local test disabled without preset code");
        const u8 selection[9] = {u8(i / 2), u8(i + 1), i, 1, 0, 0x1F, 1, 3, 0};
        Require(lobby.Confirm(selection, 100 + i), "normal lobby local confirmation");
      }
      Require(lobby.state.phase == LocalTeams::Phase::EnteringCode && !IsOnline(), "local picks searched before native code entry");
      lobby.SetSearchCode(std::string("EC\x81\x94", 4));
      Require(lobby.SearchCode() == std::string("EC\x81\x94", 4), "native Shift-JIS room code changed");
      Require(lobby.Tick(*std::make_unique<SlippiMatchmaking>(0, nullptr), nullptr) == SlippiMatchmaking::IDLE,
              "code-entry phase pretended peers were connecting");
    }
    report << "PASS: normal one/two/three/four-local lobby waits for entered code and preserves its Shift-JIS encoding\n";
    {
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount=2\nBasePort=49120\n",
                              File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini");
      LocalTeamsCoordinator ui(0, nullptr, [] { return u16(0x8); });
      ui.Poll(SlippiMatchmaking::TEAMS, buttons);
      u8 change[LocalTeams::CountPayloadSize] = {3, 2,1,0,1, 20,2,1,1, 0,0,0,0, 0,0,0,0};
      Require(ui.ChangeCount(change) && ui.state.count == 3 && ui.NativeCSS() && ui.state.saved == 3,
              "count selector did not enable native trio or retain unready placed picks");
      change[0] = 1;
      Require(ui.ChangeCount(change) && ui.state.selections[1].character == 20,
              "removing local players lost their picks");
      change[0] = 4;
      Require(ui.ChangeCount(change) && ui.state.saved == 3 && ui.NativeCSS(),
              "restoring removed players lost saved picks or disabled the native UI");
      ui.Poll(SlippiMatchmaking::TEAMS, buttons);
      for (unsigned i = 0; i < 4; ++i)
      {
        const u8 pick[9] = {u8(i / 2), u8(i + 1), 0, 1, 0, 0x1f, 1, u8(0x80 | i), 0};
        Require(ui.Confirm(pick, 100 + i), "a newly enabled native player could not ready");
        Require(ui.state.AllConfirmed() == (i == 3), "resized group opened code entry before the last player");
      }
      change[0] = 2;
      Require(!ui.ChangeCount(change) && ui.state.count == 4 && ui.state.confirmed == 15,
              "count selector changed the roster during code entry");
      ui.state.ResetSelection(); ui.validated = true;
      Require(!ui.ChangeCount(change), "count selector changed a connected rematch roster");
      ui.validated = false; ui.state.ports[1] = 0;
      Require(!ui.ChangeCount(change) && ui.state.count == 4, "count selector accepted duplicate controller ports");
    }
    report << "PASS: live count changes 1-4, saved removed players, all-player ready gate and busy/invalid roster rejection\n";
    {
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount=2\nController1=1\nController2=3\nBasePort=49120\n",
                              File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini");
      LocalTeamsCoordinator ui(0, nullptr, [] { return u16(0x8); });
      const auto initial = ui.Poll(u8(SlippiMatchmaking::TEAMS | 0x80), buttons);
      Require(initial[9] && initial[12] == 0 && initial[13] == 2 && initial[10] == 0, "native CSS config query changed input readiness or mapping");
      ui.Poll(SlippiMatchmaking::TEAMS, buttons);
      const u8 second[9] = {1, 20, 2, 1, 0, 0x1f, 1, 0x81, 0};
      const u8 first[9] = {0, 2, 1, 1, 0, 0x1f, 1, 0x80, 0};
      Require(ui.Confirm(second, 200) && !ui.state.AllConfirmed(), "native P2 confirmation requires no P1 handoff");
      const u8 back[8] = {0,0,0,0,2,0,0,0};
      auto status = ui.Poll(SlippiMatchmaking::TEAMS, back);
      Require(status[4] == 0 && status[20] == 20 && status[21] == 2 && status[22] == 1 && status[23], "native B lost P2's saved pick");
      ui.Poll(SlippiMatchmaking::TEAMS, buttons);
      Require(ui.Confirm(second, 200) && ui.Confirm(first, 100), "native local confirmations can arrive in either order");
      Require(ui.state.phase == LocalTeams::Phase::EnteringCode && !IsOnline(), "native last Start searched before code entry");
      status = ui.Poll(u8(SlippiMatchmaking::TEAMS | 0x40), buttons);
      Require(status[0] && status[9] && status[4] == 3 && !status[6] && !status[10] && !status[11],
              "keyboard polling disabled Teams or rearmed a CSS Start");
      const u8 keyboardHeld[8] = {0x10,0,0,0,0x10,0,0,0};
      ui.Poll(u8(SlippiMatchmaking::TEAMS | 0x40), keyboardHeld);
      status = ui.Poll(u8(SlippiMatchmaking::TEAMS | 0x80), buttons);
      Require(status[5] == u8(LocalTeams::Phase::EnteringCode) && !status[10], "CSS return query armed the keyboard's held Start");
      status = ui.Poll(SlippiMatchmaking::TEAMS, keyboardHeld);
      Require(!status[10], "held Start on keyboard return can reopen code entry");
      status = ui.Poll(SlippiMatchmaking::TEAMS, buttons);
      Require(status[10] == 3, "released Start after cancelled keyboard failed to rearm");
      ui.state.phase = LocalTeams::Phase::InGame; // UI-only fixture has no peer transports
      status = ui.Poll(u8(SlippiMatchmaking::TEAMS | 0x80), buttons);
      Require(status[16] == 2 && status[20] == 20 && ui.state.phase == LocalTeams::Phase::InGame, "CSS initialization discarded rematch choices or armed held inputs");
      const u8 held[8] = {0x10,0,0,0,0x10,0,0,0};
      status = ui.Poll(SlippiMatchmaking::TEAMS, held);
      Require(status[4] == 0 && status[10] == 0 && !ui.Confirm(first, 100), "held Start readied rematch player automatically");
    }
    report << "PASS: native two-player coordinator, independent ready/unready, persistent rematch choices and keyboard return/rearm guards\n";
    {
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount=1\nBasePort=49120\n",File::GetUserPath(D_CONFIG_IDX)+"local-teams.ini");
      LocalTeamsCoordinator join(0,nullptr, [] { return u16(0x8); });
      std::array<u8,48> raw{}; std::array<u8,8> sampled{};
      u32 frame=10;
      auto poll=[&](u8 mode=u8(SlippiMatchmaking::TEAMS)) {
        for(unsigned i=0;i<4;++i){ sampled[2*i]=raw[12*i]; sampled[2*i+1]=raw[12*i+1]; }
        return join.Poll(mode,sampled.data(),raw.data(),3,frame++);
      };
      auto status=poll(u8(SlippiMatchmaking::TEAMS|0x20));
      Require(status[12]==3 && status[32]==1 && status[33]==1,"port four entry did not bind/suppress only the main card");
      poll(); raw[0]=0x10;
      for(unsigned i=0;i<30;++i)status=poll();
      Require(!status[35] && status[1]==1,"Start added a card before 31 frames");
      status=poll(); Require(status[35]==0x80 && status[34]==2 && status[1]==1,"long Start did not request an atomic roster reload");
      u8 change[17]={2, 2,1,0,1};
      Require(join.ChangeCount(change) && join.state.ports[1]==0 && join.state.saved==1 && join.state.selections[0].character==2,
              "automatic join lost the existing unready pick or controller owner");
      Require(!join.state.InputEnabled(1),"joining Start leaked across the CSS reload");
      raw[2]=80; status=poll();
      Require(status[33]==2 && !(status[10]&2),"held joining Start escaped suppression or armed readiness");
      raw[0]=0; status=poll();
      Require(join.state.InputEnabled(1) && status[33]==0 && raw[2]==80,"releasing Start while holding the stick did not enable the new card");
      raw.fill(0); poll(); raw[0]=2;
      for(unsigned i=0;i<30;++i)status=poll();
      Require(!status[35],"teammate left before 31 frames");
      status=poll();
      Require(status[35]==0x41 && status[34]==1,"teammate's long B did not request removing their own card");
      change[0]=1; change[5]=20; change[6]=2; change[7]=1; change[8]=1;
      Require(join.ChangeCount(change) && join.state.count==1 && join.state.ports[0]==3 && join.state.saved==1 && join.state.selections[0].character==2,
              "teammate removal lost the main controller or its pick");
      raw.fill(0); poll(); join.validated=true; raw[12]=0x10;
      for(unsigned i=0;i<60;++i)status=poll();
      Require(!status[35] && join.state.count==1 && !join.ChangeCount(change),"connected rematch allowed a controller roster change");
      join.validated=false; Require(join.state.Resize(2),"main exit fixture could not add a waiting card");
      raw.fill(0); poll(); raw[36]=2;
      for(unsigned i=0;i<30;++i)status=poll();
      Require(!status[35],"main player exited before 31 frames");
      status=poll();
      Require(status[35]==0x40 && status[34]==0,"entering player cannot exit Teams with long B");
      change[0]=0; Require(join.ChangeCount(change) && !join.state.JoinedMask() && !join.state.saved,"main exit retained controllers or picks from the old roster");
      raw.fill(0); status=poll(u8(SlippiMatchmaking::TEAMS|0x20));
      Require(status[12]==3 && status[32]==1 && join.state.Valid(),"reentering Teams after everyone left kept invalid bindings");
    }
    report << "PASS: automatic 31-frame Start join, teammate B leave, main B-to-menu, atomic saved picks and connected-rematch guard\n";
    for(auto phase : {LocalTeams::Phase::Selecting, LocalTeams::Phase::Searching,
                      LocalTeams::Phase::Waiting, LocalTeams::Phase::EnteringCode, LocalTeams::Phase::Error})
    for(bool connected : {false,true}) for(unsigned primary=0;primary<4;++primary)
    {
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount=2\nBasePort=49120\n",File::GetUserPath(D_CONFIG_IDX)+"local-teams.ini");
      LocalTeamsCoordinator exit(0,nullptr, [] { return u16(0x8); });
      std::array<u8,48> raw{}; const u8 buttons[8]={};
      exit.Poll(u8(SlippiMatchmaking::TEAMS|0x20),buttons,raw.data(),primary,100);
      exit.Poll(SlippiMatchmaking::TEAMS,buttons,raw.data(),primary,101);
      exit.state.ClaimAddedPort((primary+1)%4);
      exit.Poll(SlippiMatchmaking::TEAMS,buttons,raw.data(),primary,102);
      exit.state.phase=phase; exit.validated=connected;
      raw[12*((primary+1)%4)]=2;
      u32 frame=103;
      if(connected || phase!=LocalTeams::Phase::Selecting)
      {
        for(unsigned i=0;i<35;++i)
          Require(!exit.Poll(SlippiMatchmaking::TEAMS,buttons,raw.data(),primary,frame++)[35],"teammate B changed a busy roster");
      }
      raw.fill(0); exit.Poll(SlippiMatchmaking::TEAMS,buttons,raw.data(),primary,frame++);
      raw[12*primary]=2;
      for(unsigned i=0;i<30;++i)
        Require(!exit.Poll(SlippiMatchmaking::TEAMS,buttons,raw.data(),primary,frame++)[35],"main B exited busy CSS before 31 frames");
      const auto pending=exit.Poll(SlippiMatchmaking::TEAMS,buttons,raw.data(),primary,frame++);
      Require(pending[35]==0x40 && pending[34]==0,"main B cannot exit searching/connected CSS");
      u8 leave[17]={};
      Require(exit.ChangeCount(leave) && exit.Active() && !exit.state.JoinedMask() && !exit.state.saved,
              "busy main exit was rejected, retained cards or disabled session cleanup");
    }
    report << "PASS: main 31-frame B hold exits idle/searching/connected CSS on every controller port; busy teammate roster changes remain blocked\n";
    for(unsigned count : {1u,2u,3u,4u}) for(bool connected : {false,true})
    {
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount="+std::to_string(count)+"\nBasePort=49120\n",File::GetUserPath(D_CONFIG_IDX)+"local-teams.ini");
      LocalTeamsCoordinator cards(0,nullptr, [] { return u16(0x8); });
      cards.Poll(SlippiMatchmaking::TEAMS,buttons);
      for(unsigned player=0;player<4;++player)
        cards.state.selections[player] = LocalTeams::Selection{u8(player+2),u8(player),u8(player%3),1,100+player};
      cards.state.saved=(1u<<count)-1;
      cards.Cleanup();
      Require(cards.state.saved==(1u<<count)-1 && cards.state.selections[0].character==2 && cards.state.selections[1].team==1,"Z cancellation unexpectedly discarded card memory");
      cards.validated=connected;
      cards.state.confirmed=(1u<<count)-1;
      const auto cleared=cards.Poll(u8(SlippiMatchmaking::TEAMS|0x20),buttons,nullptr,4);
      Require(cards.Active() && cleared[4]==0 && cleared[10]==0 && cleared[11]==0 && !cards.state.saved,"mode exit did not clear readiness or disabled cleanup of local clients");
      for(unsigned player=0;player<4;++player)
        Require(cleared[16+4*player]==0 && cleared[17+4*player]==0 && cleared[18+4*player]==0 && cleared[19+4*player]==0 && cards.state.selections[player].rng==0,"mode exit retained a remembered character, costume, team or saved pick");
    }
    report << "PASS: leaving Teams clears all four card caches in idle/connected modes; Z cancellation and normal rematches retain selections\n";
    {
      const std::string iniPath = File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini";
      File::Delete(iniPath);
      const u8 pollButtons[8] = {};
      LocalTeamsCoordinator defaults(0, nullptr, [] { return u16(0x8); });
      const auto unranked = defaults.Poll(SlippiMatchmaking::UNRANKED, pollButtons);
      Require(!defaults.Active() && unranked[0] == 0, "default local teams activated outside Teams");
      const auto teams = defaults.Poll(u8(SlippiMatchmaking::TEAMS | 0x20), pollButtons, nullptr, 0);
      Require(defaults.Active() && defaults.NativeCSS() && defaults.state.count == 1 &&
              teams[0] == 1 && teams[1] == 1 && teams[9] == 1,
              "local teams did not default to enabled native 1-P CSS when local-teams.ini is missing");
      File::WriteStringToFile("[LocalTeams]\nEnabled=False\n", iniPath);
      LocalTeamsCoordinator disabled(0, nullptr, [] { return u16(0x8); });
      const auto off = disabled.Poll(u8(SlippiMatchmaking::TEAMS | 0x20), pollButtons, nullptr, 0);
      Require(!disabled.Active() && off[0] == 0, "Enabled=False in local-teams.ini did not disable local teams");
      File::Delete(iniPath);
    }
    report << "PASS: local teams defaults to enabled native 1-P CSS when local-teams.ini is absent and respects Enabled=False\n";
    {
      const std::string iniPath = File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini";
      std::string slippiDir = File::GetSlippiUserConfigFolder();
      if (!slippiDir.empty() && slippiDir.back() != '/' && slippiDir.back() != '\\')
        slippiDir += DIR_SEP;
      const std::string testRoot = slippiDir + "selftest-identities";
      const std::string testDir = testRoot + DIR_SEP "primary";
      const std::string subJsonDir = testDir + DIR_SEP "subdir.json";
      const std::string extDir = testRoot + DIR_SEP "external";
      const std::string fewDir = testRoot + DIR_SEP "fewer";
      const std::string userJsonPath = slippiDir + "user.json";
      const std::string externalTxtPath = slippiDir + "external-user-json.txt";
      const std::string decoyPath = slippiDir + "decoy.json";

      const bool hadUserJson = File::Exists(userJsonPath);
      const bool hadExternalTxt = File::Exists(externalTxtPath);
      std::string savedUserJson, savedExternalTxt;
      if (hadUserJson)
        Require(File::ReadFileToString(userJsonPath, savedUserJson), "failed to back up existing user.json");
      if (hadExternalTxt)
        Require(File::ReadFileToString(externalTxtPath, savedExternalTxt), "failed to back up external-user-json.txt");

      struct CleanupGuard
      {
        std::string iniPath, testRoot, decoyPath, userJsonPath, externalTxtPath;
        bool hadUserJson, hadExternalTxt;
        std::string savedUserJson, savedExternalTxt;
        ~CleanupGuard()
        {
          File::Delete(iniPath);
          File::Delete(decoyPath);
          if (!testRoot.empty())
            File::DeleteDirRecursively(testRoot);
          if (hadUserJson)
            File::WriteStringToFile(savedUserJson, userJsonPath);
          else
            File::Delete(userJsonPath);
          if (hadExternalTxt)
            File::WriteStringToFile(savedExternalTxt, externalTxtPath);
          else
            File::Delete(externalTxtPath);
        }
      } cleanup{iniPath,     testRoot,      decoyPath,     userJsonPath,
                externalTxtPath, hadUserJson, hadExternalTxt, savedUserJson, savedExternalTxt};

      Require(File::CreateFullPath(testDir + DIR_SEP), "failed to create identity autodiscovery dir");
      Require(File::CreateFullPath(subJsonDir + DIR_SEP), "failed to create subdir.json");
      Require(File::CreateFullPath(extDir + DIR_SEP), "failed to create external dir");
      Require(File::CreateFullPath(fewDir + DIR_SEP), "failed to create fewer dir");

      auto makeJson = [](const std::string& uid, const std::string& pk, const std::string& name,
                         const std::string& code, bool snakeCase = false) {
        if (snakeCase)
        {
          return "{\"uid\":\"" + uid + "\",\"play_key\":\"" + pk + "\",\"display_name\":\"" + name +
                 "\",\"connect_code\":\"" + code + "\"}";
        }
        return "{\"uid\":\"" + uid + "\",\"playKey\":\"" + pk + "\",\"displayName\":\"" + name +
               "\",\"connectCode\":\"" + code + "\"}";
      };

      SlippiUser::UserInfo fallback;
      fallback.uid = "uid-main";
      fallback.playKey = "pk-main";
      fallback.displayName = "Main";
      fallback.connectCode = "MAIN#1";

      Require(File::WriteStringToFile(makeJson("uid-main", "pk-main", "MainFile", "MF#0"), testDir + DIR_SEP "user.json"),
              "write user.json");
      Require(File::WriteStringToFile("[{\"connectCode\":\"FOO#1\"}]", testDir + DIR_SEP "direct-codes.json"),
              "write direct-codes.json");
      Require(File::WriteStringToFile("{\"volume\":50}", testDir + DIR_SEP "settings.json"), "write settings.json");
      Require(File::WriteStringToFile("{broken json", testDir + DIR_SEP "malformed.json"), "write malformed.json");
      Require(File::WriteStringToFile(makeJson("uid-empty-pk", "", "EmptyPK", "EP#1"), testDir + DIR_SEP "empty_pk.json"),
              "write empty_pk.json");
      Require(File::WriteStringToFile(makeJson("", "pk-empty-uid", "EmptyUID", "EU#1"), testDir + DIR_SEP "empty_uid.json"),
              "write empty_uid.json");
      Require(File::WriteStringToFile(makeJson("uid-main", "pk-dup-main", "DupMain", "DM#1"),
                                      testDir + DIR_SEP "a_dup_main.json"),
              "write a_dup_main.json");
      Require(File::WriteStringToFile(makeJson("uid-nested", "pk-nested", "Nested", "NST#1"),
                                      subJsonDir + DIR_SEP "inside.json"),
              "write subdir.json/inside.json");
      Require(File::WriteStringToFile(makeJson("uid-txt", "pk-txt", "Txt", "TXT#1"), testDir + DIR_SEP "readme.txt"),
              "write readme.txt");
      Require(File::WriteStringToFile(makeJson("uid-alpha", "pk-alpha", "Alpha", "ALP#1"), testDir + DIR_SEP "Alpha.json"),
              "write Alpha.json");
      Require(File::WriteStringToFile(makeJson("uid-alpha", "pk-dup-alpha", "DupAlpha", "DAL#1"),
                                      testDir + DIR_SEP "az_dup_alpha.json"),
              "write az_dup_alpha.json");
      Require(File::WriteStringToFile(makeJson("uid-bravo", "pk-bravo", "Bravo", "BRV#2", true),
                                      testDir + DIR_SEP "bravo.JSON"),
              "write bravo.JSON");
      Require(File::WriteStringToFile(makeJson("uid-charlie", "pk-charlie", "Charlie", "CHL#3"),
                                      testDir + DIR_SEP "charlie.json"),
              "write charlie.json");

      // 1. Pure autodiscovery for 4 players with logged-in fallback.
      File::Delete(iniPath);
      LocalTeamsCoordinator pure(0, nullptr, [] { return u16(0x8); });
      pure.state.count = 4;
      std::array<SlippiUser::UserInfo, 4> resolved;
      std::string identityErr;
      Require(pure.ResolveIdentities(fallback, resolved, identityErr, testDir),
              ("pure 4-player autodiscovery failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-main" && resolved[1].uid == "uid-alpha" && resolved[2].uid == "uid-bravo" &&
                  resolved[2].displayName == "Bravo" && resolved[2].connectCode == "BRV#2" &&
                  resolved[3].uid == "uid-charlie",
              "pure 4-player autodiscovery produced unexpected slot order");

      // 2. Removing the duplicate-of-main file leaves 3 unique non-user identities, so empty fallback fails loudly.
      File::Delete(testDir + DIR_SEP "a_dup_main.json");
      SlippiUser::UserInfo emptyFallback;
      Require(!pure.ResolveIdentities(emptyFallback, resolved, identityErr, testDir) &&
                  identityErr == "Log in before searching",
              "expected Log in before searching when fallback is empty and only 3 unique identities exist");

      // 3. Adding a 4th discovered file succeeds with empty fallback.
      Require(File::WriteStringToFile(makeJson("uid-delta", "pk-delta", "Delta", "DLT#4"), testDir + DIR_SEP "delta.json"),
              "write delta.json");
      Require(pure.ResolveIdentities(emptyFallback, resolved, identityErr, testDir),
              ("4-player autodiscovery without fallback failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-alpha" && resolved[1].uid == "uid-bravo" && resolved[2].uid == "uid-charlie" &&
                  resolved[3].uid == "uid-delta",
              "4-player autodiscovery without fallback order mismatch");

      // 4. Explicit Identity2 inside discovery dir avoids duplicate UID in remaining discovered slots.
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nIdentity2=" + testDir + DIR_SEP "charlie.json\n", iniPath);
      LocalTeamsCoordinator explicitInDir(0, nullptr, [] { return u16(0x8); });
      explicitInDir.state.count = 4;
      Require(explicitInDir.ResolveIdentities(fallback, resolved, identityErr, testDir),
              ("explicit Identity2 in discovery dir failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-main" && resolved[1].uid == "uid-charlie" && resolved[2].uid == "uid-alpha" &&
                  resolved[3].uid == "uid-bravo",
              "explicit Identity2 in discovery dir order mismatch");

      // 5. Explicit Identity1 and Identity2 outside discovery dir take precedence; Identity2 claiming fallback UID
      // suppresses duplicating fallback into P1.
      const std::string ext1Path = extDir + DIR_SEP "p1_override.json";
      const std::string ext2Path = extDir + DIR_SEP "p2_override.json";
      Require(File::WriteStringToFile(makeJson("uid-ext1", "pk-ext1", "Ext1", "EX#1"), ext1Path), "write p1_override.json");
      Require(File::WriteStringToFile(makeJson("uid-ext2", "pk-ext2", "Ext2", "EX#2"), ext2Path), "write p2_override.json");
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nIdentity1=" + ext1Path + "\nIdentity2=" + ext2Path + "\n",
                              iniPath);
      LocalTeamsCoordinator explicitExt(0, nullptr, [] { return u16(0x8); });
      explicitExt.state.count = 4;
      Require(explicitExt.ResolveIdentities(fallback, resolved, identityErr, testDir),
              ("explicit external overrides failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-ext1" && resolved[1].uid == "uid-ext2" && resolved[2].uid == "uid-alpha" &&
                  resolved[3].uid == "uid-bravo",
              "explicit external overrides order mismatch");

      const std::string extMainDupPath = extDir + DIR_SEP "main_alias.json";
      Require(File::WriteStringToFile(makeJson("uid-main", "pk-main", "MainAlias", "MA#1"), extMainDupPath),
              "write main_alias.json");
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nIdentity2=" + extMainDupPath + "\n", iniPath);
      LocalTeamsCoordinator claimedFallback(0, nullptr, [] { return u16(0x8); });
      claimedFallback.state.count = 4;
      Require(claimedFallback.ResolveIdentities(fallback, resolved, identityErr, testDir),
              ("explicit Identity2 claiming fallback UID failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-alpha" && resolved[1].uid == "uid-main" && resolved[2].uid == "uid-bravo" &&
                  resolved[3].uid == "uid-charlie",
              "explicit Identity2 claiming fallback UID still assigned fallback to P1");

      // 6. Missing or unreadable configured identities fall back to autodiscovery, then to primary user.
      const std::string emptyDir = testRoot + DIR_SEP "empty";
      Require(File::CreateFullPath(emptyDir + DIR_SEP), "failed to create empty discovery dir");

      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nIdentity1=" + extDir + DIR_SEP "missing.json\n", iniPath);
      LocalTeamsCoordinator missingId1(0, nullptr, [] { return u16(0x8); });
      missingId1.state.count = 2;
      Require(missingId1.ResolveIdentities(fallback, resolved, identityErr, testDir),
              ("missing Identity1 fallback to autodiscovery failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-alpha" && resolved[1].uid == "uid-bravo",
              "missing Identity1 did not fall back to autodiscovered identities");

      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nIdentity2=" + extDir + DIR_SEP "missing.json\n", iniPath);
      LocalTeamsCoordinator missingId2(0, nullptr, [] { return u16(0x8); });
      missingId2.state.count = 3;
      Require(missingId2.ResolveIdentities(fallback, resolved, identityErr, testDir),
              ("missing Identity2 fallback to autodiscovery failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-main" && resolved[1].uid == "uid-alpha" && resolved[2].uid == "uid-bravo",
              "missing Identity2 did not fall back to autodiscovered identities");

      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nIdentity2=" + testDir + DIR_SEP "malformed.json\n", iniPath);
      LocalTeamsCoordinator malformedId2(0, nullptr, [] { return u16(0x8); });
      malformedId2.state.count = 2;
      Require(malformedId2.ResolveIdentities(fallback, resolved, identityErr, testDir),
              ("malformed Identity2 fallback to autodiscovery failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-main" && resolved[1].uid == "uid-alpha",
              "malformed Identity2 did not fall back to autodiscovered identity");

      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nIdentity2=" + subJsonDir + "\n", iniPath);
      LocalTeamsCoordinator dirId2(0, nullptr, [] { return u16(0x8); });
      dirId2.state.count = 2;
      Require(dirId2.ResolveIdentities(fallback, resolved, identityErr, testDir),
              ("directory Identity2 fallback to autodiscovery failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-main" && resolved[1].uid == "uid-alpha",
              "directory Identity2 did not fall back to autodiscovered identity");

      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nIdentity1=" + extDir + DIR_SEP "missing1.json\nIdentity2=" +
                                  extDir + DIR_SEP "missing2.json\n",
                              iniPath);
      LocalTeamsCoordinator missingAllNoDiscovery(0, nullptr, [] { return u16(0x8); });
      missingAllNoDiscovery.state.count = 2;
      Require(missingAllNoDiscovery.ResolveIdentities(fallback, resolved, identityErr, emptyDir),
              ("missing identities without autodiscovery failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-main" && resolved[1].uid == "uid-main",
              "missing identities without autodiscovery did not fall back to primary user");
      Require(!missingAllNoDiscovery.ResolveIdentities(emptyFallback, resolved, identityErr, emptyDir) &&
                  identityErr == "Log in before searching",
              "missing identities without autodiscovery or login did not require login");

      // 7. Fewer discovered files fall back to primary user for remaining slots.
      File::Delete(iniPath);
      Require(File::WriteStringToFile(makeJson("uid-only", "pk-only", "Only", "ONL#1"), fewDir + DIR_SEP "only.json"),
              "write only.json");
      LocalTeamsCoordinator fewer(0, nullptr, [] { return u16(0x8); });
      fewer.state.count = 3;
      Require(fewer.ResolveIdentities(fallback, resolved, identityErr, fewDir),
              ("fewer discovered identities failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-main" && resolved[1].uid == "uid-only" && resolved[2].uid == "uid-main",
              "fewer discovered identities did not fall back to primary user");

      // 8. external-user-json.txt parent-directory resolution (without explicit discoveryDir).
      const std::string extTargetDir = testRoot + DIR_SEP "external-target";
      Require(File::CreateFullPath(extTargetDir + DIR_SEP), "failed to create external-target dir");
      File::Delete(userJsonPath);
      Require(File::WriteStringToFile(makeJson("uid-decoy", "pk-decoy", "Decoy", "DCY#1"), decoyPath), "write decoy.json");
      Require(File::WriteStringToFile(makeJson("uid-ext-user", "pk-ext-user", "ExtUser", "EXU#1"),
                                      extTargetDir + DIR_SEP "user.json"),
              "write external user.json");
      Require(File::WriteStringToFile(makeJson("uid-partner", "pk-partner", "Partner", "PRT#2"),
                                      extTargetDir + DIR_SEP "partner.json"),
              "write external partner.json");
      const std::string extTxtBody = std::string("\xEF\xBB\xBF  ") + extTargetDir + DIR_SEP "user.json \r\n";
      Require(File::WriteStringToFile(extTxtBody, externalTxtPath), "write external-user-json.txt");

      File::Delete(iniPath);
      LocalTeamsCoordinator viaExternalTxt(0, nullptr, [] { return u16(0x8); });
      viaExternalTxt.state.count = 2;
      Require(viaExternalTxt.ResolveIdentities(fallback, resolved, identityErr),
              ("external-user-json.txt discovery failed: " + identityErr).c_str());
      Require(resolved[0].uid == "uid-main" && resolved[1].uid == "uid-partner",
              "external-user-json.txt did not discover from external user.json directory");
    }
    report << "PASS: local teams identity autodiscovery respects explicit overrides and falls back cleanly on invalid/missing files\n";
  }
  catch (const std::exception& error) { report << "FAIL: " << error.what() << '\n'; result = 1; }
  enet_deinitialize();
  return result;
}
