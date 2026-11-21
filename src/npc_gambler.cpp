/*
 * Asmongold <Deathroll Addict> for Tofu Worldview.
 * Fork of azerothcore/mod-npc-gambler (originally by StygianTheBest, ported by gtao725), rewritten for Tofu Worldview.
 * Released under the GNU AGPL v3, as the original.
 *
 * Games: Gamba (1-100 roll with pity and jackpot), Rock-Paper-Scissors (1 copper per win), Deathroll (player vs Asmongold, 0 loses).
 * Design: claude/asmongold-npc.md in the Tofu Worldview project.
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "StringConvert.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // npc_text IDs (registry: npc_text 900020 to 900029)
    enum AsmonTexts : uint32
    {
        TEXT_MAIN             = 900020,
        TEXT_GAMBA            = 900021,
        TEXT_RPS              = 900022,
        TEXT_DR_BET           = 900023,
        TEXT_DR_TYPE          = 900024,
        TEXT_DR_READY         = 900025,
        TEXT_DR_ASMON_TURN    = 900026,
        TEXT_DR_PLAYER_TURN   = 900027
    };

    enum AsmonActions : uint32
    {
        ACTION_MAIN              = 1,
        ACTION_GAMBA_MENU        = 2,
        ACTION_RPS_MENU          = 3,
        ACTION_DR_MENU           = 4,
        ACTION_GAMBA_BET_BASE    = 100, // + 0..3
        ACTION_RPS_BASE          = 200, // + 0 rock, 1 paper, 2 scissors
        ACTION_DR_BET_BASE       = 300, // + 0..3
        ACTION_DR_TYPE_SUDDEN    = 400,
        ACTION_DR_TYPE_ROLLIE    = 401,
        ACTION_DR_TYPE_STANDARD  = 402,
        ACTION_DR_TYPE_CUSTOM    = 403,
        ACTION_DR_START          = 500,
        ACTION_DR_NEVERMIND      = 501,
        ACTION_DR_ASMON_GO       = 502,
        ACTION_DR_PLAYER_TURN    = 503
    };

    enum AsmonSpellsSounds : uint32
    {
        SPELL_POLYMORPH_CHICKEN  = 228,
        SPELL_FEAR               = 5782,
        SPELL_WIN_VISUAL         = 47292, // upstream win effect
        SPELL_JACKPOT_VISUAL     = 44940, // upstream jackpot effect
        SOUND_GOBLIN_PISSED      = 5960,
        SOUND_JACKPOT            = 3337
    };

    constexpr uint32 COPPER_PER_GOLD = 10000;
    constexpr size_t BET_COUNT = 4;

    struct AsmonConfig
    {
        std::array<uint32, BET_COUNT> gambaBets{ 1000, 10000, 250000, 1000000 }; // copper
        uint32 gambaWinMax = 49;      // rolls 1..gambaWinMax win, 100 is the jackpot, the rest lose
        uint32 gambaWinMult = 2;      // total returned on a win, as a multiple of the stake
        uint32 gambaJackpotMult = 3;  // total returned on a jackpot
        uint32 pityLosses = 5;        // consecutive losses (all players) before the pity roll
        uint32 pityBonus = 25;        // subtracted from the next losing roll
        std::array<uint32, BET_COUNT> drBetsGold{ 10, 100, 500, 1000 };
        uint32 drCustomMin = 1;
        uint32 drCustomMax = 1000000;
        uint32 rpsWinCopper = 1;
        bool barkerEnable = true;
        uint32 barkerMinMs = 60000;
        uint32 barkerMaxMs = 180000;
        std::vector<std::string> barkerMessages;
    };

    enum class DrPhase : uint8
    {
        ChoosingType,  // bet picked, nothing paid yet
        ReadyToStart,  // paid, waiting for "I start!" or "nevermind"
        AsmonTurn,     // waiting for "Your go!"
        PlayerTurn     // waiting for "My turn..."
    };

    struct DeathrollState
    {
        DrPhase phase = DrPhase::ChoosingType;
        uint32 betGold = 0;
        uint32 current = 0; // the next roll is 0..current
    };

    // Shared state. Gossip and AI updates can run on different threads, so everything below is guarded by sMutex.
    std::mutex sMutex;
    AsmonConfig sConfig;
    std::unordered_map<ObjectGuid, DeathrollState> sGames;
    uint32 sGambaLossStreak = 0; // global by design

    AsmonConfig GetConfig()
    {
        std::lock_guard<std::mutex> lock(sMutex);
        return sConfig;
    }

    std::string Trim(std::string const& s)
    {
        size_t const first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        size_t const last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    std::vector<std::string> SplitMessages(std::string const& all)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= all.size())
        {
            size_t const end = all.find(';', start);
            std::string const piece = Trim(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (!piece.empty())
                out.push_back(piece);
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return out;
    }

    std::string FormatMoney(uint64 copper)
    {
        uint64 const g = copper / 10000;
        uint64 const s = (copper % 10000) / 100;
        uint64 const c = copper % 100;
        std::string out;
        if (g)
            out += std::to_string(g) + "g";
        if (s)
            out += (out.empty() ? "" : " ") + std::to_string(s) + "s";
        if (c || out.empty())
            out += (out.empty() ? "" : " ") + std::to_string(c) + "c";
        return out;
    }

    // True if the player can hold this much more money without passing the gold cap.
    bool FitsUnderCap(Player* player, uint64 extra)
    {
        return uint64(player->GetMoney()) + extra <= uint64(MAX_MONEY_AMOUNT);
    }

    void Punish(Player* player, Creature* creature, std::string const& message)
    {
        CloseGossipMenuFor(player);
        creature->Whisper(message, LANG_UNIVERSAL, player);
        player->AddAura(SPELL_POLYMORPH_CHICKEN, player);
        player->AddAura(SPELL_FEAR, player);
        player->PlayDirectSound(SOUND_GOBLIN_PISSED, player);
        creature->HandleEmoteCommand(EMOTE_ONESHOT_RUDE);
    }

    std::string const& PickOne(std::vector<std::string> const& lines)
    {
        return lines[urand(0, uint32(lines.size() - 1))];
    }

    // ---------- Menus ----------

    void ShowMain(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Deathroll", GOSSIP_SENDER_MAIN, ACTION_DR_MENU);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Gamba", GOSSIP_SENDER_MAIN, ACTION_GAMBA_MENU);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Rock-Paper-Scissors", GOSSIP_SENDER_MAIN, ACTION_RPS_MENU);
        SendGossipMenuFor(player, TEXT_MAIN, creature->GetGUID());
    }

    void ShowGamba(Player* player, Creature* creature)
    {
        AsmonConfig const cfg = GetConfig();
        ClearGossipMenuFor(player);
        for (size_t i = 0; i < BET_COUNT; ++i)
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Bet " + FormatMoney(cfg.gambaBets[i]), GOSSIP_SENDER_MAIN, ACTION_GAMBA_BET_BASE + uint32(i));
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        SendGossipMenuFor(player, TEXT_GAMBA, creature->GetGUID());
    }

    void ShowRps(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Rock", GOSSIP_SENDER_MAIN, ACTION_RPS_BASE + 0);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Paper", GOSSIP_SENDER_MAIN, ACTION_RPS_BASE + 1);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Scissors", GOSSIP_SENDER_MAIN, ACTION_RPS_BASE + 2);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        SendGossipMenuFor(player, TEXT_RPS, creature->GetGUID());
    }

    void ShowDrBet(Player* player, Creature* creature)
    {
        AsmonConfig const cfg = GetConfig();
        ClearGossipMenuFor(player);
        for (size_t i = 0; i < BET_COUNT; ++i)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Bet " + std::to_string(cfg.drBetsGold[i]) + "g", GOSSIP_SENDER_MAIN, ACTION_DR_BET_BASE + uint32(i));
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        SendGossipMenuFor(player, TEXT_DR_BET, creature->GetGUID());
    }

    void ShowDrType(Player* player, Creature* creature, uint32 betGold)
    {
        AsmonConfig const cfg = GetConfig();
        std::string const range = std::to_string(cfg.drCustomMin) + "-" + std::to_string(cfg.drCustomMax);
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Sudden Death (1)", GOSSIP_SENDER_MAIN, ACTION_DR_TYPE_SUDDEN);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Rollie (100)", GOSSIP_SENDER_MAIN, ACTION_DR_TYPE_ROLLIE);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Standard (" + std::to_string(uint64(betGold) * 10) + ")", GOSSIP_SENDER_MAIN, ACTION_DR_TYPE_STANDARD);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Custom...", GOSSIP_SENDER_MAIN, ACTION_DR_TYPE_CUSTOM, "Type the number to roll to (" + range + ").", 0, true);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_DR_MENU);
        SendGossipMenuFor(player, TEXT_DR_TYPE, creature->GetGUID());
    }

    void ShowDrReady(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "I start!", GOSSIP_SENDER_MAIN, ACTION_DR_START);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Actually, nevermind...", GOSSIP_SENDER_MAIN, ACTION_DR_NEVERMIND);
        SendGossipMenuFor(player, TEXT_DR_READY, creature->GetGUID());
    }

    // The 3.3.5a client auto-selects a gossip menu's only option unless its icon is GOSSIP_ICON_CHAT, so the single-option turn menus below must use the chat icon to wait for a click.
    void ShowDrAsmonTurn(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Your go!", GOSSIP_SENDER_MAIN, ACTION_DR_ASMON_GO);
        SendGossipMenuFor(player, TEXT_DR_ASMON_TURN, creature->GetGUID());
    }

    void ShowDrPlayerTurn(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "My turn...", GOSSIP_SENDER_MAIN, ACTION_DR_PLAYER_TURN);
        SendGossipMenuFor(player, TEXT_DR_PLAYER_TURN, creature->GetGUID());
    }

    // ---------- Gamba ----------

    void DoGamba(Player* player, Creature* creature, size_t index)
    {
        AsmonConfig const cfg = GetConfig();
        uint32 const bet = cfg.gambaBets[index];

        if (!player->HasEnoughMoney(bet))
        {
            Punish(player, creature, "Hey, I got no time for cheapskates, " + player->GetName() + ". Come back when you have " + FormatMoney(bet) + "!");
            return;
        }

        // A jackpot must fit under the gold cap, or the client would drop the winnings.
        if (!FitsUnderCap(player, uint64(bet) * cfg.gambaJackpotMult - bet))
        {
            creature->Whisper("You're too rich to gamba, " + player->GetName() + ". Spend some gold first.", LANG_UNIVERSAL, player);
            ShowGamba(player, creature);
            return;
        }

        player->ModifyMoney(-int32(bet));

        uint32 roll = urand(1, 100);
        bool pityUsed = false;
        enum class Outcome { Lose, Win, Jackpot } outcome;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            bool const losing = roll > cfg.gambaWinMax && roll < 100;
            if (losing && sGambaLossStreak >= cfg.pityLosses)
            {
                roll = roll > cfg.pityBonus ? roll - cfg.pityBonus : 1;
                pityUsed = true;
            }

            if (roll == 100)
                outcome = Outcome::Jackpot;
            else if (roll <= cfg.gambaWinMax)
                outcome = Outcome::Win;
            else
                outcome = Outcome::Lose;

            // Any win resets the streak; a pity roll is used up whether it wins or not.
            if (outcome != Outcome::Lose || pityUsed)
                sGambaLossStreak = 0;
            else
                ++sGambaLossStreak;
        }

        if (pityUsed)
            creature->Whisper("Lady luck isn't on your side tonight, " + player->GetName() + ". I'll knock " + std::to_string(cfg.pityBonus) + " off that one.", LANG_UNIVERSAL, player);

        creature->Whisper("The dice come to rest at " + std::to_string(roll) + " (1-100).", LANG_UNIVERSAL, player);

        ChatHandler handler(player->GetSession());
        switch (outcome)
        {
            case Outcome::Jackpot:
            {
                uint32 const payout = bet * cfg.gambaJackpotMult;
                player->ModifyMoney(int32(payout));
                player->PlayDirectSound(SOUND_JACKPOT, player);
                player->CastSpell(player, SPELL_WIN_VISUAL, true);
                player->CastSpell(player, SPELL_JACKPOT_VISUAL, true);
                player->GetSession()->SendAreaTriggerMessage("JACKPOT! " + player->GetName() + " wins " + FormatMoney(payout) + "!");
                handler.SendSysMessage("JACKPOT! You won " + FormatMoney(payout) + ".");
                creature->HandleEmoteCommand(EMOTE_ONESHOT_APPLAUD);
                break;
            }
            case Outcome::Win:
            {
                uint32 const payout = bet * cfg.gambaWinMult;
                player->ModifyMoney(int32(payout));
                player->CastSpell(player, SPELL_WIN_VISUAL, true);
                handler.SendSysMessage("You won " + FormatMoney(payout) + ".");
                creature->HandleEmoteCommand(EMOTE_ONESHOT_APPLAUD);
                break;
            }
            case Outcome::Lose:
                handler.SendSysMessage("You lost " + FormatMoney(bet) + ".");
                creature->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
                break;
        }

        ShowGamba(player, creature);
    }

    // ---------- Rock-Paper-Scissors ----------

    void DoRps(Player* player, Creature* creature, uint32 choice)
    {
        static char const* const throws[3] = { "Rock", "Paper", "Scissors" };
        static std::vector<std::string> const winLines = { "WHAT? No way. Fine, take your copper.", "That's rigged. Whatever. One copper, don't spend it all at once.", "I let you have that one. Obviously." };
        static std::vector<std::string> const tieLines = { "Same throw? Again. AGAIN!", "Great minds. Go again.", "A tie? Nobody wins. Run it back." };
        static std::vector<std::string> const loseLines = { "Get rolled. Too easy.", "Predictable. Everybody throws that.", "Ez. Free win." };

        AsmonConfig const cfg = GetConfig();
        uint32 const outcome = urand(0, 2); // 0 player wins, 1 tie, 2 player loses
        uint32 asmon;
        if (outcome == 0)
            asmon = (choice + 2) % 3; // the throw the player's choice beats
        else if (outcome == 1)
            asmon = choice;
        else
            asmon = (choice + 1) % 3; // the throw that beats the player's choice

        std::string const lead = std::string(throws[asmon]) + "! ";
        ChatHandler handler(player->GetSession());
        if (outcome == 0)
        {
            creature->Say(lead + PickOne(winLines), LANG_UNIVERSAL);
            creature->HandleEmoteCommand(EMOTE_ONESHOT_CRY);
            player->ModifyMoney(int32(cfg.rpsWinCopper));
            handler.SendSysMessage("You threw " + std::string(throws[choice]) + " and won " + FormatMoney(cfg.rpsWinCopper) + ".");
        }
        else if (outcome == 1)
        {
            creature->Say(lead + PickOne(tieLines), LANG_UNIVERSAL);
            creature->HandleEmoteCommand(EMOTE_ONESHOT_QUESTION);
            handler.SendSysMessage("You threw " + std::string(throws[choice]) + ". Tie.");
        }
        else
        {
            creature->Say(lead + PickOne(loseLines), LANG_UNIVERSAL);
            creature->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
            handler.SendSysMessage("You threw " + std::string(throws[choice]) + " and lost.");
        }

        ShowRps(player, creature);
    }

    // ---------- Deathroll ----------

    bool GetGame(Player* player, DeathrollState& out)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        auto it = sGames.find(player->GetGUID());
        if (it == sGames.end())
            return false;
        out = it->second;
        return true;
    }

    void SetGame(Player* player, DeathrollState const& state)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sGames[player->GetGUID()] = state;
    }

    void EraseGame(ObjectGuid const& guid)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sGames.erase(guid);
    }

    void SelectDrBet(Player* player, Creature* creature, size_t index)
    {
        AsmonConfig const cfg = GetConfig();
        DeathrollState state;
        state.phase = DrPhase::ChoosingType;
        state.betGold = cfg.drBetsGold[index];
        SetGame(player, state);
        ShowDrType(player, creature, state.betGold);
    }

    // Takes the bet and moves to ReadyToStart, rolling to n.
    void StartDeathroll(Player* player, Creature* creature, uint32 n)
    {
        DeathrollState state;
        if (!GetGame(player, state) || state.phase != DrPhase::ChoosingType)
        {
            ShowMain(player, creature);
            return;
        }

        uint64 const bet = uint64(state.betGold) * COPPER_PER_GOLD;
        if (bet > uint64(MAX_MONEY_AMOUNT) || !player->HasEnoughMoney(uint32(bet)))
        {
            EraseGame(player->GetGUID());
            Punish(player, creature, "Hey, I got no time for cheapskates, " + player->GetName() + ". Come back when you have " + std::to_string(state.betGold) + " gold!");
            return;
        }

        // A win returns 2x the bet; it must fit under the gold cap.
        if (!FitsUnderCap(player, bet))
        {
            EraseGame(player->GetGUID());
            creature->Whisper("You're too rich to deathroll, " + player->GetName() + ". Spend some gold first.", LANG_UNIVERSAL, player);
            ShowMain(player, creature);
            return;
        }

        player->ModifyMoney(-int32(bet));
        state.phase = DrPhase::ReadyToStart;
        state.current = n;
        SetGame(player, state);

        creature->Whisper(std::to_string(state.betGold) + " gold on the line, rolling to " + std::to_string(n) + ". You start.", LANG_UNIVERSAL, player);
        ShowDrReady(player, creature);
    }

    void DeathrollPlayerLost(Player* player, Creature* creature, uint32 betGold)
    {
        static std::vector<std::string> const lines = { "GET DEATHROLLED.", "Thanks for the gold. Come again.", "It's never the dice. It's you." };
        creature->Say(PickOne(lines), LANG_UNIVERSAL);
        creature->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
        ChatHandler(player->GetSession()).SendSysMessage("You lost the deathroll and " + std::to_string(betGold) + "g.");
    }

    void DeathrollPlayerWon(Player* player, Creature* creature, uint32 betGold)
    {
        static std::vector<std::string> const lines = { "NO. NO NO NO. It's over. Take it.", "That's actually rigged. Whatever. Take your gold.", "I was carrying the whole time and THAT happens?" };
        uint64 const payout = uint64(betGold) * COPPER_PER_GOLD * 2;
        if (!player->ModifyMoney(int32(payout)))
            ChatHandler(player->GetSession()).SendSysMessage("Your winnings could not be paid (gold cap). Report this to a GM.");
        else
            ChatHandler(player->GetSession()).SendSysMessage("You won the deathroll: " + FormatMoney(payout) + ".");
        player->CastSpell(player, SPELL_WIN_VISUAL, true);
        creature->Say(PickOne(lines), LANG_UNIVERSAL);
        creature->HandleEmoteCommand(EMOTE_ONESHOT_CRY);
    }

    // Player rolls 0..current (from ReadyToStart or PlayerTurn).
    void DoPlayerRoll(Player* player, Creature* creature, DrPhase expected)
    {
        DeathrollState state;
        if (!GetGame(player, state) || state.phase != expected)
        {
            ShowMain(player, creature);
            return;
        }

        uint32 const n = state.current;
        uint32 const m = urand(0, n);
        creature->TextEmote(player->GetName() + " rolls " + std::to_string(m) + " (0-" + std::to_string(n) + ") against " + creature->GetName() + ".");

        if (m == 0)
        {
            EraseGame(player->GetGUID());
            DeathrollPlayerLost(player, creature, state.betGold);
            ShowMain(player, creature);
            return;
        }

        state.current = m;
        state.phase = DrPhase::AsmonTurn;
        SetGame(player, state);
        ShowDrAsmonTurn(player, creature);
    }

    void DoAsmonRoll(Player* player, Creature* creature)
    {
        DeathrollState state;
        if (!GetGame(player, state) || state.phase != DrPhase::AsmonTurn)
        {
            ShowMain(player, creature);
            return;
        }

        uint32 const n = state.current;
        uint32 const a = urand(0, n);
        creature->TextEmote(creature->GetName() + " rolls " + std::to_string(a) + " (0-" + std::to_string(n) + ") against " + player->GetName() + ".");

        if (a == 0)
        {
            EraseGame(player->GetGUID());
            DeathrollPlayerWon(player, creature, state.betGold);
            ShowMain(player, creature);
            return;
        }

        state.current = a;
        state.phase = DrPhase::PlayerTurn;
        SetGame(player, state);
        ShowDrPlayerTurn(player, creature);
    }

    void DoNevermind(Player* player, Creature* creature)
    {
        DeathrollState state;
        if (!GetGame(player, state) || state.phase != DrPhase::ReadyToStart)
        {
            ShowMain(player, creature);
            return;
        }

        EraseGame(player->GetGUID());
        player->ModifyMoney(int32(uint64(state.betGold) * COPPER_PER_GOLD));
        Punish(player, creature, "Backing out now? Here's your gold back, " + player->GetName() + ". Cheapskate.");
    }

    // Validates the Custom number; re-asks on bad input.
    void SelectDrCustom(Player* player, Creature* creature, char const* code)
    {
        DeathrollState state;
        if (!GetGame(player, state) || state.phase != DrPhase::ChoosingType)
        {
            ShowMain(player, creature);
            return;
        }

        AsmonConfig const cfg = GetConfig();
        std::string const text = Trim(code ? code : "");
        bool digitsOnly = !text.empty() && text.size() <= 10;
        for (char ch : text)
            if (ch < '0' || ch > '9')
                digitsOnly = false;

        Optional<uint32> const value = digitsOnly ? Acore::StringTo<uint32>(text) : Optional<uint32>();
        if (!value || *value < cfg.drCustomMin || *value > cfg.drCustomMax)
        {
            creature->Whisper("That's not a number I can roll. Pick a whole number from " + std::to_string(cfg.drCustomMin) + " to " + std::to_string(cfg.drCustomMax) + ".", LANG_UNIVERSAL, player);
            ShowDrType(player, creature, state.betGold);
            return;
        }

        StartDeathroll(player, creature, *value);
    }
}

class AsmongoldConfig : public WorldScript
{
public:
    AsmongoldConfig() : WorldScript("AsmongoldConfig", {
        WORLDHOOK_ON_BEFORE_CONFIG_LOAD
    }) { }

    // Runs at startup and on .reload config (the conf files are re-read before this hook).
    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        AsmonConfig cfg;
        cfg.gambaBets[0] = sConfigMgr->GetOption<uint32>("Asmongold.Gamba.Bet1", 1000);
        cfg.gambaBets[1] = sConfigMgr->GetOption<uint32>("Asmongold.Gamba.Bet2", 10000);
        cfg.gambaBets[2] = sConfigMgr->GetOption<uint32>("Asmongold.Gamba.Bet3", 250000);
        cfg.gambaBets[3] = sConfigMgr->GetOption<uint32>("Asmongold.Gamba.Bet4", 1000000);
        cfg.gambaWinMax = sConfigMgr->GetOption<uint32>("Asmongold.Gamba.WinMax", 49);
        cfg.gambaWinMult = sConfigMgr->GetOption<uint32>("Asmongold.Gamba.WinMultiplier", 2);
        cfg.gambaJackpotMult = sConfigMgr->GetOption<uint32>("Asmongold.Gamba.JackpotMultiplier", 3);
        cfg.pityLosses = sConfigMgr->GetOption<uint32>("Asmongold.Gamba.PityLosses", 5);
        cfg.pityBonus = sConfigMgr->GetOption<uint32>("Asmongold.Gamba.PityBonus", 25);
        cfg.drBetsGold[0] = sConfigMgr->GetOption<uint32>("Asmongold.Deathroll.Bet1", 10);
        cfg.drBetsGold[1] = sConfigMgr->GetOption<uint32>("Asmongold.Deathroll.Bet2", 100);
        cfg.drBetsGold[2] = sConfigMgr->GetOption<uint32>("Asmongold.Deathroll.Bet3", 500);
        cfg.drBetsGold[3] = sConfigMgr->GetOption<uint32>("Asmongold.Deathroll.Bet4", 1000);
        cfg.drCustomMin = sConfigMgr->GetOption<uint32>("Asmongold.Deathroll.CustomMin", 1);
        cfg.drCustomMax = sConfigMgr->GetOption<uint32>("Asmongold.Deathroll.CustomMax", 1000000);
        cfg.rpsWinCopper = sConfigMgr->GetOption<uint32>("Asmongold.RPS.WinCopper", 1);
        cfg.barkerEnable = sConfigMgr->GetOption<bool>("Asmongold.Barker.Enable", true);
        cfg.barkerMinMs = sConfigMgr->GetOption<uint32>("Asmongold.Barker.IntervalMin", 60) * IN_MILLISECONDS;
        cfg.barkerMaxMs = sConfigMgr->GetOption<uint32>("Asmongold.Barker.IntervalMax", 180) * IN_MILLISECONDS;
        cfg.barkerMessages = SplitMessages(sConfigMgr->GetOption<std::string>("Asmongold.Barker.Messages", ""));

        // Sanity limits so a bad conf value cannot break the games.
        for (uint32& bet : cfg.gambaBets)
            bet = std::clamp<uint32>(bet, 1, 100000000); // 1c to 10,000g
        for (uint32& bet : cfg.drBetsGold)
            bet = std::clamp<uint32>(bet, 1, 100000); // 1g to 100,000g
        cfg.gambaWinMax = std::min<uint32>(cfg.gambaWinMax, 99);
        cfg.gambaWinMult = std::clamp<uint32>(cfg.gambaWinMult, 1, 10);
        cfg.gambaJackpotMult = std::clamp<uint32>(cfg.gambaJackpotMult, 1, 10);
        cfg.drCustomMin = std::max<uint32>(cfg.drCustomMin, 1);
        cfg.drCustomMax = std::clamp<uint32>(cfg.drCustomMax, cfg.drCustomMin, 1000000000);
        cfg.barkerMinMs = std::max<uint32>(cfg.barkerMinMs, 10 * IN_MILLISECONDS);
        cfg.barkerMaxMs = std::max<uint32>(cfg.barkerMaxMs, cfg.barkerMinMs);

        std::lock_guard<std::mutex> lock(sMutex);
        sConfig = std::move(cfg);
    }
};

class AsmongoldPlayer : public PlayerScript
{
public:
    AsmongoldPlayer() : PlayerScript("AsmongoldPlayer", {
        PLAYERHOOK_ON_LOGOUT
    }) { }

    // A deathroll in progress is forfeited on logout.
    void OnPlayerLogout(Player* player) override
    {
        EraseGame(player->GetGUID());
    }
};

class npc_asmongold : public CreatureScript
{
public:
    npc_asmongold() : CreatureScript("gamble_npc") { }

    struct npc_asmongoldAI : public ScriptedAI
    {
        npc_asmongoldAI(Creature* creature) : ScriptedAI(creature) { }

        uint32 barkTimer = 0;

        uint32 NextInterval() const
        {
            AsmonConfig const cfg = GetConfig();
            return urand(cfg.barkerMinMs, cfg.barkerMaxMs);
        }

        void Reset() override
        {
            barkTimer = NextInterval();
        }

        void UpdateAI(uint32 diff) override
        {
            if (barkTimer > diff)
            {
                barkTimer -= diff;
                return;
            }

            AsmonConfig const cfg = GetConfig();
            if (cfg.barkerEnable && !cfg.barkerMessages.empty())
            {
                me->Say(PickOne(cfg.barkerMessages), LANG_UNIVERSAL);
                me->HandleEmoteCommand(EMOTE_ONESHOT_EXCLAMATION);
            }
            barkTimer = urand(cfg.barkerMinMs, cfg.barkerMaxMs);
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_asmongoldAI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        // Reopening the window mid-deathroll forfeits it (the bet was already taken).
        bool forfeited = false;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            auto it = sGames.find(player->GetGUID());
            if (it != sGames.end())
            {
                forfeited = it->second.phase != DrPhase::ChoosingType;
                sGames.erase(it);
            }
        }

        if (forfeited)
        {
            creature->Whisper("You walked away from a deathroll, " + player->GetName() + "? That gold is mine now.", LANG_UNIVERSAL, player);
            creature->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
        }

        ShowMain(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        ClearGossipMenuFor(player);
        if (sender != GOSSIP_SENDER_MAIN)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        if (action >= ACTION_GAMBA_BET_BASE && action < ACTION_GAMBA_BET_BASE + BET_COUNT)
        {
            DoGamba(player, creature, action - ACTION_GAMBA_BET_BASE);
            return true;
        }
        if (action >= ACTION_RPS_BASE && action < ACTION_RPS_BASE + 3)
        {
            DoRps(player, creature, action - ACTION_RPS_BASE);
            return true;
        }
        if (action >= ACTION_DR_BET_BASE && action < ACTION_DR_BET_BASE + BET_COUNT)
        {
            SelectDrBet(player, creature, action - ACTION_DR_BET_BASE);
            return true;
        }

        switch (action)
        {
            case ACTION_GAMBA_MENU:
                ShowGamba(player, creature);
                break;
            case ACTION_RPS_MENU:
                ShowRps(player, creature);
                break;
            case ACTION_DR_MENU:
            {
                // Back from the roll-type menu drops the unpaid choice.
                DeathrollState state;
                if (GetGame(player, state) && state.phase == DrPhase::ChoosingType)
                    EraseGame(player->GetGUID());
                ShowDrBet(player, creature);
                break;
            }
            case ACTION_DR_TYPE_SUDDEN:
                StartDeathroll(player, creature, 1);
                break;
            case ACTION_DR_TYPE_ROLLIE:
                StartDeathroll(player, creature, 100);
                break;
            case ACTION_DR_TYPE_STANDARD:
            {
                DeathrollState state;
                if (!GetGame(player, state))
                {
                    ShowMain(player, creature);
                    break;
                }
                StartDeathroll(player, creature, state.betGold * 10);
                break;
            }
            case ACTION_DR_START:
                DoPlayerRoll(player, creature, DrPhase::ReadyToStart);
                break;
            case ACTION_DR_NEVERMIND:
                DoNevermind(player, creature);
                break;
            case ACTION_DR_ASMON_GO:
                DoAsmonRoll(player, creature);
                break;
            case ACTION_DR_PLAYER_TURN:
                DoPlayerRoll(player, creature, DrPhase::PlayerTurn);
                break;
            case ACTION_MAIN:
            default:
            {
                DeathrollState state;
                if (GetGame(player, state) && state.phase == DrPhase::ChoosingType)
                    EraseGame(player->GetGUID());
                ShowMain(player, creature);
                break;
            }
        }
        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action, char const* code) override
    {
        ClearGossipMenuFor(player);
        if (sender != GOSSIP_SENDER_MAIN || action != ACTION_DR_TYPE_CUSTOM)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        SelectDrCustom(player, creature, code);
        return true;
    }
};

void AddNPCGamblerScripts()
{
    new AsmongoldConfig();
    new AsmongoldPlayer();
    new npc_asmongold();
}
