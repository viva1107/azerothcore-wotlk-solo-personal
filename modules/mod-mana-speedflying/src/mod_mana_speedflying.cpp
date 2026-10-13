/*
 * mod-mana-speedflying: vary a flying mount's speed with its flight state.
 *
 * Server-side companion to the client's wxl-mana-speedflying module. That module already works out the
 * state (climbing / leveling / diving) and whether the mount carries the AdvFly clips, so it
 * says so directly: it whispers an addon message with the "WXLMS" prefix, and this module blends the
 * mount's flight speed toward the speed the state calls for. No server-side guesswork and no mount
 * list -- a mount the client does not drive with AdvFly reports '0' and its speed is left alone.
 *
 * Protocol, an addon whisper:  "WXLMS\t<advfly:0|1><state:L|C|D|H>"
 *   advfly  '1' = the mount is one wxl-mana-speedflying drives (AdvFly clips), '0' = leave the speed alone
 *   state   C climb, D dive, H hovering, L level / strafe / backward
 *
 * The server answers with its own configured speeds so the client scales its bar, particles and shake
 * to them:  "WXLMS\tS:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,<stacks>,
 * <stackMs>,<stackDiveMs>"  (the same WXLMS prefix; the client reads the last addon message once a
 * second). <cooldownMs> is sent as 0 (the boost has no cooldown) and the two trailing intervals are the
 * normal and diving stack-recharge cadences. <stacks> is the server's live boost-stack count, so the
 * client button mirrors the authoritative value instead of drifting; it is re-sent whenever the count
 * changes.
 *
 * The state is applied over a transition whose length and curve depend on where the speed is going:
 * a dive accelerates into DiveSpeed (faster and faster), pulling out to level is a long glide that
 * bleeds speed slowly at first and then hard, and pulling up into a climb holds the built-up speed
 * before braking hard. A climb cut short to level keeps that glide. Other hops keep the
 * span-proportional blend. If the link goes quiet for TimeoutMs, the
 * mount's own speed is restored.
 *
 * The talent tree rides a second addon prefix, "WXLTT", so the link parser above is untouched:
 *
 *   client -> server:  WXLTT\tW\t<talentId>   learn one rank
 *                      WXLTT\tR\t<1|2|3|A>    refund a branch (or A for everything)
 *                      WXLTT\tQ                request the tree
 *   server -> client:  WXLTT\tT\t<points>,<spent>,<unspent>[,<id>:<rank>,...][,P<permille>,N<yards>]
 *                      WXLTT\tP\t<permille>,<yards>   the partial point alone, for the progress bar
 *                      WXLTT\tF\t<reason>     a refusal, echoed for the panel
 *
 * The tally rides a spend, a refund or a 'Q' -- anything that can move a rank. The partial point moves
 * on its own, about twice a second while flight accrues yards, and answering that with a whole tally
 * (every rank pair, several hundred bytes) is what made the module chatty. 'P' carries the two bar
 * fields and nothing else, so the same information costs tens of bytes. A client that does not know
 * 'P' ignores the body and keeps the bar it last drew.
 *
 * Points are earned per character by flying: yards travelled on an AdvFly mount while the link is
 * up, at Talents.YardsPerPoint per whole point, capped per rolling minute. Ranks are authoritative
 * here and the client only renders them. See the README for the talent list and the wire contract.
 */

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // --- the talent tree -----------------------------------------------------------------------
    // The ids are the wire contract: they ride "WXLTT" as integers and key the
    // character_speedflying_talents table. Append new talents at the end and never renumber -- a
    // stored rank would silently move to a different talent.
    enum Branch
    {
        kBranchDive = 0,
        kBranchGlide,
        kBranchWings,
        kBranchCount
    };

    // Every talent scales one leaf of Config. The base value stays the existing ManaSpeedFlying.*
    // key and the talent is a percentage of it, so a talent mints no config key of its own.
    // D2 and W15 both scale E_ClimbDecel: their ranks are summed into one multiplier, never
    // compounded one after the other.
    enum Effect
    {
        E_DiveMs = 0,          // D1 Steep Descent
        E_ClimbDecel,          // D2 Held Breath, W15 Rising Current
        E_DiveSpeed,           // D3 Terminal Velocity
        E_ShallowPull,         // D4 Shallow Pull: degrees past 30 a pull-out may be and still keep its glide
        E_GlideMs,             // G6 Long Glide
        E_LevelElev,           // G10 Skyward Path: how far above LevelSpeed a glide may end
        E_YardsPerPoint,       // G7 Slipstream
        E_BoostCarry,          // G8 Banked Momentum
        E_BoostStacks,         // G9 Twin Vault
        E_StackInterval,       // W12 Steady Wings
        E_StackDiveInterval,   // W11 Diving Wings
        E_BoostAdd,            // W13 Iron Wings
        E_Feel,                // W14 Smoothed Plumage (client-side only)
        E_Count
    };

    constexpr int kEffectCount = static_cast<int>(E_Count);

    struct TalentDef
    {
        uint16      id;
        uint8       branch;
        uint8       tier;     // 1..4, top to bottom
        uint8       maxRank;
        uint8       effect;   // leaf of Config the talent scales; E_Feel is client-side only
        float       pctPerRank;
        const char* name;
        const char* desc;
    };

    // Cost is 1 point per rank. Tiers 1 to 3 are three ranks each and tier 4 is a single-rank capstone,
    // so filling one branch costs 10 of the 13 points a character can ever earn. A tier opens only once
    // every tier above it in the same branch is at its full rank, and only one talent of a (branch, tier)
    // group may hold a rank: that exclusivity is the tier-2 pick. Both rules live in HandleTalentLearn.
    // The two 1-point bridges at tier 3 are exempt from both, and Dive is the entry branch every branch
    // hangs off; see HandleTalentLearn for the exact order a write is validated in.
    // Append-only and never renumber -- the ids key the stored ranks -- and the client panel lays itself
    // out from these entries.
    const TalentDef kTalents[] =
    {
        // Dive
        { 1, kBranchDive, 1, 3, E_DiveMs,           -0.08f, "Steep Descent",
          "Accelerate into a dive 8% faster per rank." },
        { 3, kBranchDive, 2, 3, E_DiveSpeed,         0.06f, "Terminal Velocity",
          "Reach 6% more dive speed per rank." },
        { 2, kBranchDive, 2, 3, E_ClimbDecel,        0.15f, "Held Breath",
          "Hold dive speed 15% longer per rank when pulling up into a climb." },
        { 4, kBranchDive, 3, 3, E_ShallowPull,      15.0f, "Shallow Pull",
          "A pull-out 15 degrees shallower per rank keeps its whole glide." },
        { 5, kBranchDive, 4, 1, E_Count,             0.0f, "Aftershock",
          "Releasing the boost key into a dive commits the burst at once." },
        // Glide
        { 6, kBranchGlide, 1, 3, E_GlideMs,          0.12f, "Long Glide",
          "Glide 12% longer per rank out of a dive." },
        { 7, kBranchGlide, 2, 3, E_YardsPerPoint,    0.12f, "Slipstream",
          "Shorten the step to the next point by 12% per rank." },
        { 8, kBranchGlide, 2, 3, E_BoostCarry,       0.10f, "Banked Momentum",
          "Fold 10 more points of a boost into the glide per rank." },
        { 10, kBranchGlide, 3, 3, E_LevelElev,       0.10f, "Skyward Path",
          "Glide 10% further above the level bar per rank." },
        { 9, kBranchGlide, 4, 1, E_BoostStacks,      1.0f,  "Twin Vault",
          "Hold a fourth boost stack." },
        // Wings
        { 14, kBranchWings, 1, 3, E_Feel,            -0.25f, "Smoothed Plumage",
          "Dive shake 25% quieter per rank, speed motes 30% brighter." },
        { 12, kBranchWings, 2, 3, E_StackInterval,  -0.08f, "Steady Wings",
          "Boost stacks recharge 8% faster per rank." },
        { 11, kBranchWings, 2, 3, E_StackDiveInterval, -0.10f, "Diving Wings",
          "Diving boost stacks recharge 10% faster per rank." },
        { 13, kBranchWings, 3, 3, E_BoostAdd,         0.08f, "Iron Wings",
          "Each boost adds 8% more speed per rank." },
        { 15, kBranchWings, 4, 1, E_ClimbDecel,       0.40f, "Rising Current",
          "A climb keeps 40% more of its entered speed." },
        // The two bridges. One point each, and the rank is what opens the next branch (GateOpens): a
        // talent in a closed branch is refused with "tree-locked". They sit beside the branch's own
        // tier-3 talent but are excluded from TierDone and from the tier-pick rule, so the tier still
        // completes on its one real talent and the capstone below it stays reachable -- and Steep
        // Descent plus Still Air is legal instead of a rival pick. E_Count means "no server-side
        // effect": the angle is a client-side threshold applied from the rank in the tally, exactly
        // like 14 Smoothed Plumage.
        // 10 degrees is the value: the level band is 15, and 15 + 10 = 25 is still clear of the 35
        // that starts a dive, so one rank widens the glide without eating the dive band.
        { 16, kBranchDive,  3, 1, E_Count, 0.0f, "Still Air",
          "Glide 10 degrees steeper before it reads as a climb, and open the Glide branch." },
        { 17, kBranchGlide, 3, 1, E_Count, 0.0f, "Tucked Wings",
          "Shallow the dive threshold 10 degrees, and open the Wings branch." },
    };

    constexpr int kTalentCount = static_cast<int>(sizeof(kTalents) / sizeof(kTalents[0]));

    // The bridge ids are the wire contract like every other id, and they are the unlock graph: a rank
    // in 16 opens kBranchGlide, a rank in 17 opens kBranchWings. Dive is always open, so buying a
    // bridge never needs the branch it opens to be open first.
    constexpr uint16 kBridgeGlideId = 16;
    constexpr uint16 kBridgeWingsId = 17;

    // The branch a bridge opens, or kBranchCount for an ordinary talent.
    uint8 GateOpens(uint16 id)
    {
        if (id == kBridgeGlideId) return kBranchGlide;
        if (id == kBridgeWingsId) return kBranchWings;
        return kBranchCount;
    }

    bool IsBridge(int index)
    {
        return index >= 0 && index < kTalentCount && GateOpens(kTalents[index].id) != kBranchCount;
    }

    enum class Motion
    {
        Level = 0,
        Climb,
        Dive,
        Hover
    };

    // Shape of a transition's speed curve.
    enum class Curve
    {
        Linear, // constant rate
        EaseIn, // holds the start speed, then moves hard near the end
        Dive    // starts with a small kick, then keeps accelerating
    };

    struct Config
    {
        bool   enabled      = true;
        float  climbSpeed   = 2.0f;  // 200% of base run speed
        float  levelSpeed   = 2.8f;  // 280%
        float  diveSpeed    = 8.0f;  // 800%
        uint32 blendMs      = 5500;  // default blend, proportional to the span it must cover
        uint32 diveMs       = 4000;  // accelerate into a dive
        uint32 glideMs      = 18000; // long ease-in glide from a dive back to level
        uint32 climbDecelMs = 3500;  // dive -> climb: hold the dive speed, then brake hard
        uint32 hoverMs      = 500;   // short accel/decel zone leaving a hover
        uint32 sendMs       = 100;   // how often the blended speed may reach the client
        uint32 timeoutMs    = 2500;  // drop the link if no message arrives for this long
        float  boostAdd     = 4.0f;  // 400%: the speed one boost adds back to the glide
        float  boostSpeed   = 16.0f; // 1600%: hard ceiling a boost may lift the speed to
        uint32 boostFadeMs  = 4000;  // how fast a boost's added speed bleeds back to the state speed
        float  boostCarryPct = 0.50f; // fraction of a boost folded into the glide to outlast the fade
        float  boostGlidePct = 0.70f; // fraction of the ordinary glide a boost's carried speed may buy
        int    boostStacks  = 3;     // most boost stacks the button can hold
        uint32 stackMs      = 9000;  // ms between boost stacks in normal flight
        uint32 stackDiveMs  = 3000;  // ms between boost stacks while diving
        // Talent-derived, never read from the INI: D4 sets the angle a pull-out may be and still keep
        // its whole glide, G10 the rate a level-out settles to before it drops to LevelSpeed.
        float  shallowPullDeg = 30.0f;
        float  levelElev      = 0.0f;
        uint32 settleMs       = 1500;  // G10: how long the elevated level-out takes to fall back to LevelSpeed
    };

    // Talent-tree configuration. Talents.Enable is the master switch for earning and spending;
    // the effects are applied through an effective per-player Config, never by mutating g_cfg.
    struct TalentConfig
    {
        bool   enabled        = true;
        uint32 maxPoints      = 13;     // lifetime cap: a full 10-point branch, both 1-point bridges, and the
                                        // point they open in the branch next door
        float  baseYards      = 500.0f; // yards of flight for the first point
        float  yardStep       = 1.00f;  // each point after the first needs twice the yards of the one before
        uint32 maxPointsPerMinute = 4;  // rolling-window cap; 0 disables (an AFK orbit is free)
        uint32 flushMs        = 20000;  // how often a dirty progress map is written back
        float  diveAccrual    = 1.5f;   // yards count this much more while diving -- diving pays best
        float  teleportYards  = 200.0f; // a tick delta past this is a teleport, not flight
        uint32 respecCost     = 0;      // copper per respec; 0 (the default) is free
    };

    Config g_cfg;
    TalentConfig g_talentCfg;

    // Multipliers the tree produces, recomputed from a rank set. Read by the award loop and by the
    // per-player effective config, never by g_cfg.
    struct Talents
    {
        int   ranks[kTalentCount] = {};
        float eff[kEffectCount];

        Talents() { for (int i = 0; i < kEffectCount; ++i) eff[i] = 1.0f; }

        int Spent() const { int n = 0; for (int i = 0; i < kTalentCount; ++i) n += ranks[i]; return n; }

        // Tiers open top down. A tier is done when a *full* talent has been bought inside it -- 3 points
        // in the same skill, which is what the tier-2 pick needs: only one rival may hold ranks, so a
        // full pick is 3 spent either way. Requiring one rank instead let a single point open every tier
        // below it, which is what the tree used to do.
        bool TierDone(int branch, uint8 tier) const
        {
            int spent = 0;
            int quota = 0;
            for (int i = 0; i < kTalentCount; ++i)
            {
                if (kTalents[i].branch != branch || kTalents[i].tier != tier)
                    continue;
                if (IsBridge(i))
                    continue;
                spent += ranks[i];
                if (quota == 0)
                    quota = kTalents[i].maxRank;
            }
            // The client's TierDone applies the same rule, so the two cannot disagree about what is
            // clickable and what is accepted.
            return quota > 0 && spent >= quota;
        }

        // The unlock graph: Dive is the entry branch and is always open; kBranchGlide opens on a rank
        // in the Dive bridge (16) and kBranchWings on a rank in the Glide bridge (17). A bridge is a
        // gate, not a bypass, so a node whose own branch is closed is refused too -- see HandleTalentLearn.
        bool BranchOpen(int branch) const
        {
            if (branch <= kBranchDive || branch >= kBranchCount)
                return true;
            for (int i = 0; i < kTalentCount; ++i)
                if (GateOpens(kTalents[i].id) == branch && ranks[i] > 0)
                    return true;
            return false;
        }
    };

    // The challenge curve lives here once and is read by the award loop. A point's Slipstream bonus
    // scales its effective requirement rather than the rate yards are banked at, so a rank in it can
    // only ever shorten the tree. `index` is the point being paid for: the 0th point costs BaseYards.
    double PointYards(uint32 index, const Talents& t)
    {
        double need = static_cast<double>(g_talentCfg.baseYards);
        for (uint32 i = 0; i < index; ++i)
            need *= 1.0 + static_cast<double>(g_talentCfg.yardStep);
        const float slip = t.eff[E_YardsPerPoint];
        return slip > 0.01f ? need / static_cast<double>(slip) : need;
    }

    // The progress bar's fill, in permille of the point being paid for. yardPartial is a yard count,
    // not a fraction, so it is the next point's own cost that it is measured against. A tree at the
    // cap has no next point: the bar is full. One definition, so the tally and the bar tick agree.
    uint32 PartialPermille(uint32 points, double yardPartial, const Talents& t)
    {
        if (points >= g_talentCfg.maxPoints) return 1000u;
        const double need = PointYards(points, t);
        if (need <= 0.0) return 1000u;
        const double q = yardPartial / need * 1000.0;
        return q <= 0.0 ? 0u : (q >= 1000.0 ? 1000u : static_cast<uint32>(q));
    }

    // The shared cap on a talent-raised state speed, as a percentage. Applied to the three base state
    // speeds *before* boostSpeed is derived from their max(), or the derivation undoes the clamp. Read
    // through ApplyTalents' capPct argument, never directly by a caller.
    float g_speedCapPct = 1600.0f;

    // A player's flush timer fired. The write itself rides the world thread's next tick, outside
    // g_lock: the update path must not block on the character database while it holds the lock.
    std::atomic<bool> g_flushWanted = false;

    // Rebuilds a rank set's multipliers from the table: every effect starts at 1.0 and takes
    // pctPerRank * rank. Two talents on one effect sum (Held Breath + Rising Current).
    void BuildTalents(const int ranks[kTalentCount], Talents& out)
    {
        out = Talents();
        for (int i = 0; i < kTalentCount; ++i)
        {
            const int r = std::max(0, std::min<int>(ranks[i], kTalents[i].maxRank));
            out.ranks[i] = r;
            if (!r)
                continue;
            if (kTalents[i].effect < E_Count)
                out.eff[kTalents[i].effect] += kTalents[i].pctPerRank * r;
        }
        // A negative multiplier would invert a duration into a negative one.
        for (int i = 0; i < kEffectCount; ++i)
            out.eff[i] = std::max(0.0f, out.eff[i]);
        // W14 is a client-side feel talent: its ranks are announced, never applied here.
        out.eff[E_Feel] = 1.0f;
    }


    // The same three overrides, applied to a per-player copy. diveSpeed is raised before boostSpeed is
    // derived from it, which is the whole point of clamping the base states and not the ceiling.
    // capPct is passed in, not read from g_speedCapPct: a caller that is not holding g_lock (the login
    // thread) would otherwise read the shared config while a reload is writing it.
    void ApplyTalents(Config& c, const Talents& t, float capPct)
    {
        c.diveSpeed  *= t.eff[E_DiveSpeed];
        c.diveMs      = static_cast<uint32>(static_cast<float>(c.diveMs) * t.eff[E_DiveMs]);
        c.glideMs     = static_cast<uint32>(static_cast<float>(c.glideMs) * t.eff[E_GlideMs]);
        c.climbDecelMs = static_cast<uint32>(static_cast<float>(c.climbDecelMs) * t.eff[E_ClimbDecel]);
        c.boostAdd   *= t.eff[E_BoostAdd];
        c.boostCarryPct = std::min(1.0f, c.boostCarryPct + (t.eff[E_BoostCarry] - 1.0f));
        c.boostStacks = c.boostStacks + static_cast<int>(t.eff[E_BoostStacks] - 1.0f + 0.5f);
        c.stackMs     = static_cast<uint32>(static_cast<float>(c.stackMs) * t.eff[E_StackInterval]);
        c.stackDiveMs = static_cast<uint32>(static_cast<float>(c.stackDiveMs) * t.eff[E_StackDiveInterval]);
        // G10 Skyward Path raises the rate a level-out settles to, so the glide ends above the level
        // bar and the mount then waits out `settleMs` before it drops the rest of the way.
        c.levelElev = c.levelSpeed * t.eff[E_LevelElev];

        // One shared cap, applied to the base states before the ceiling is derived from them.
        const float cap = capPct * 0.01f;
        c.climbSpeed = std::min(c.climbSpeed, cap);
        c.levelSpeed = std::min(c.levelSpeed, cap);
        c.diveSpeed  = std::min(c.diveSpeed,  cap);
        // D4 is additive degrees, not a percentage: 30 degrees plus 15 per rank.
        c.shallowPullDeg = 30.0f + (t.eff[E_ShallowPull] - 1.0f);
        // The elevated glide is held to the same cap: a raised LevelSpeed still bounds it.
        c.levelElev  = std::min(c.levelElev, cap);
        c.boostSpeed = std::max(c.boostSpeed,
                                std::max(c.climbSpeed, std::max(c.levelSpeed, c.diveSpeed)));

        if (!c.diveMs) c.diveMs = 1;
        if (!c.glideMs) c.glideMs = 1;
        if (!c.climbDecelMs) c.climbDecelMs = 1;
        if (!c.stackMs) c.stackMs = 1;
        if (!c.stackDiveMs) c.stackDiveMs = 1;
        if (c.boostStacks > 6) c.boostStacks = 6; // 3 configured + 3 from G9 Twin Vault
    }

    // --- per-character talent progress -------------------------------------------------------
    // Unspent points are derived (points - sum of ranks), never stored twice. yard_partial carries
    // the fraction under a whole point across a flush and across a relog.
    struct Progress
    {
        Talents  tree;                      // the rank set and its multipliers
        Config   eff;                       // g_cfg with this player's talents applied
        uint32   points     = 0;            // lifetime points earned
        double   yardPartial = 0.0;         // yards not yet worth a whole point
        bool     dirty      = false;        // something to write back
        bool     loaded     = false;        // the two tables have been read
        uint32   accMs      = 0;            // ms since the last flush check
        // The rolling minute's cap counts the points paid inside it, oldest first, so the sample
        // that leaves the window is the one dropped. Halving the count every minute instead (the
        // earlier decay) held an unbroken flight at half the configured rate.
        std::vector<uint32> recentMs;       // age in ms of each point paid inside the window
        // The partial point the progress bar last showed (a permille), so a bar tick that moved it can
        // push a tally once. A points change forces one regardless.
        uint32   shownPartial = 0;
        uint32   shownPoints  = 0;
        uint32   progMs       = 0;            // ms since the progress bar was last re-announced

        // The flight position the previous accrual tick saw, so a tick pays the distance it covered.
        bool     havePos    = false;
        float    lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;

        int Spent() const { return tree.Spent(); }
        int Unspent() const
        {
            const int n = static_cast<int>(points) - Spent();
            return n > 0 ? n : 0;
        }
        // Both base values are passed in rather than read from the globals: LoadProgress runs on the
        // login thread, and g_cfg/g_speedCapPct are only writable under g_lock.
        void Rebuild(const Config& base, float capPct)
        {
            // Copy the ranks first: BuildTalents resets `tree` (including tree.ranks), so passing the
            // live array across would zero the very ranks it is about to read. And start the effective
            // config from the base every time -- applying talents on top of an already-talented copy
            // would compound them on every rank bought.
            int ranks[kTalentCount];
            for (int i = 0; i < kTalentCount; ++i)
                ranks[i] = tree.ranks[i];
            BuildTalents(ranks, tree);
            eff = base;                       // the base ...
            ApplyTalents(eff, tree, capPct);  // ... then this character's ranks on top
        }
    };

    // The character guid the two tables key on is the guid's *low* half: `characters.guid` is
    // GetGUID().GetCounter(), which for a player is exactly the low 32 bits of GetRawValue() (a player
    // guid has no entry bits). Keying the SQL on the raw value instead would never match a row.
    uint32 DbGuid(uint64 raw) { return static_cast<uint32>(raw & 0xFFFFFFFFull); }

    // A module cannot add a CharacterDatabasePreparedStatement entry -- that enum lives in the core.
    // These queries interpolate integers only (guids, talent ids, ranks), never a string.
    // `base` and `capPct` are an already-taken copy of the published config. This runs on the login
    // thread, before anything is under g_lock: reading the globals here and letting OnAfterConfigLoad
    // assign them would be an unsynchronized race. The caller copies under the lock instead.
    void LoadProgress(uint64 guid, const Config& base, float capPct, Progress& p)
    {
        p.points = 0;
        p.yardPartial = 0.0;
        p.shownPartial = 0;
        p.shownPoints = 0;
        p.tree = Talents();

        if (QueryResult r = CharacterDatabase.Query(
                "SELECT `points`, `yard_partial` FROM `character_speedflying` WHERE `guid` = {}",
                DbGuid(guid)))
        {
            if (Field* f = r->Fetch())
            {
                p.points      = f[0].Get<uint32>();
                p.yardPartial = f[1].Get<double>();
            }
        }

        if (QueryResult r = CharacterDatabase.Query(
                "SELECT `talent_id`, `rank` FROM `character_speedflying_talents` WHERE `guid` = {}",
                DbGuid(guid)))
        {
            do
            {
                Field* f = r->Fetch();
                if (!f)
                    break;
                const int id = f[0].Get<uint16>();
                const int rank = f[1].Get<uint8>();
                for (int i = 0; i < kTalentCount; ++i)
                    if (kTalents[i].id == id)
                        p.tree.ranks[i] = std::max(0, std::min<int>(rank, kTalents[i].maxRank));
            } while (r->NextRow());
        }

        p.Rebuild(base, capPct);
        p.dirty  = false;
        p.loaded = true;
    }

    // Writes the progress row, the ranks that changed and nothing else. Clears `dirty` on whatever it
    // is handed: a caller writing a snapshot under the lock must clear the live flag itself.
    void WriteProgress(uint64 guid, Progress& p)
    {
        CharacterDatabase.Execute(
            "INSERT INTO `character_speedflying` (`guid`, `points`, `yard_partial`) VALUES ({}, {}, {}) "
            "ON DUPLICATE KEY UPDATE `points` = VALUES(`points`), `yard_partial` = VALUES(`yard_partial`)",
            DbGuid(guid), p.points, p.yardPartial);

        // Talent rows exist only while rank >= 1; a respec deletes them. Rewriting the whole (small)
        // set keeps the delete path honest without tracking which row moved.
        CharacterDatabase.Execute("DELETE FROM `character_speedflying_talents` WHERE `guid` = {}",
                                   DbGuid(guid));
        for (int i = 0; i < kTalentCount; ++i)
        {
            if (p.tree.ranks[i] <= 0)
                continue;
            CharacterDatabase.Execute(
                "INSERT INTO `character_speedflying_talents` (`guid`, `talent_id`, `rank`) VALUES ({}, {}, {})",
                DbGuid(guid), static_cast<uint32>(kTalents[i].id), static_cast<uint32>(p.tree.ranks[i]));
        }
        p.dirty = false;
    }

    struct PlayerState
    {
        bool   eligible  = false;         // client said the mount is AdvFly-capable
        bool   fromDive  = false;         // the running climb was entered straight out of a dive
        Motion motion    = Motion::Level; // state the current transition is heading to
        float  start     = 0.0f;          // rate when the current transition began
        float  target    = 0.0f;          // rate the current state calls for
        float  current   = 0.0f;          // rate being applied
        float  lastSent  = -1.0f;         // last rate handed to SetSpeed; < 0 = nothing applied
        uint32 elapsed   = 0;             // ms into the current transition
        uint32 duration  = 0;             // ms the current transition runs for; 0 = instant
        Curve  curve     = Curve::Linear; // shape of the current transition
        uint32 sendAccum = 0;
        uint32 sinceLink = 0;             // ms since the last link message, for the timeout
        bool   announced = false;         // the client has been told the current speeds
        uint32 lastAnnounce = 0;          // ms of the last speed announce, for the slow resend
        uint32 lastMsgMs = 0;             // ms of the last non-boost state message, for the flood guard
        int    stacks = 0;                // boost stacks held (to g_cfg.boostStacks)
        uint32 stackAccum = 0;            // ms accrued toward the next boost stack
        uint32 stackInterval = 0;         // cadence stackAccum was accrued at; lets a switch rescale
        // A boost is an additive offset on the running transition, not a new transition of its own:
        // the state blend keeps running underneath and the offset fades to nothing, so the speed
        // lands back on the glide the mount already had instead of snapping to the plain state speed.
        float  boostStart = 0.0f;         // the offset a boost added, before its fade
        float  boostOffset = 0.0f;        // the still-decaying part of that offset
        uint32 boostElapsed = 0;          // ms into the fade
        uint32 boostDuration = 0;         // ms the fade runs for; 0 = no boost running
        float  pendingCarry = 0.0f;       // boost carry banked while climbing, released when the climb ends
        float  divePitch  = 0.0f;         // degrees below the horizon of the last dive tick (D4 reads it)
        bool   settling   = false;        // G10: the glide landed on the elevated rate, settling to Level now
        float  settleFrom = 0.0f;         // rate that settle started at
        uint32 settleAccum = 0;           // ms into that settle
    };

    std::mutex g_lock;
    std::unordered_map<uint64, PlayerState> g_states;
    std::unordered_map<uint64, Progress>    g_progress;

    // The per-player effective config, built once per update and never mutated in place. Returned by
    // value on purpose: a Progress lives in a map another player's message may be inserting into.
    Config EffectiveConfig(uint64 guid)
    {
        auto it = g_progress.find(guid);
        return it != g_progress.end() ? it->second.eff : g_cfg;
    }

    // Flushes every dirty progress map while g_lock is held: a per-tick caller would block the whole
    // link-tick path on the character database, so the tick only raises g_flushWanted and the write
    // rides the world thread's own tick, which does not hold the lock. Logout and shutdown keep the
    // synchronous call -- they need the rows on disk before the process or the session goes away.
    void FlushAllProgress(bool force = false)
    {
        for (auto& kv : g_progress)
        {
            Progress& p = kv.second;
            if (p.loaded && (p.dirty || force))
                WriteProgress(kv.first, p);
        }
    }

    // The same flush with no lock held. The world thread's tick calls this: it copies the dirty rows
    // under the lock, releases it, and only then talks to the database, so a flushed character DB
    // cannot stall another player's chat hook behind the mutex. Writing a snapshot cannot be overtaken:
    // this flush, every other write and every erase run on the world thread -- the login thread only
    // ever inserts into the map, which is why the write works from a copy.
    void FlushAllProgressUnlocked()
    {
        std::vector<std::pair<uint64, Progress>> rows;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            rows.reserve(g_progress.size());
            for (auto& kv : g_progress)
                if (kv.second.loaded && kv.second.dirty)
                {
                    // Clear the flag on the live entry: WriteProgress clears it on the copy it is
                    // handed, so a write that skipped this would leave the row dirty for every tick
                    // until it happened to land inside the lock again.
                    kv.second.dirty = false;
                    rows.emplace_back(kv.first, kv.second);
                }
        }
        // Writes run free of the lock; WriteProgress only needs its own copy.
        for (auto& r : rows)
            WriteProgress(r.first, r.second);
    }

    // Scales a fixed transition time by how far the speed actually has to travel, relative to the
    // configured full drop. A shallow pull-out recovers sooner than a deep one; the canonical dive
    // (current at the far rate) keeps the configured duration exactly.
    uint32 ScaledMs(uint32 baseMs, float dist, float reference)
    {
        if (reference <= 0.0f)
            return baseMs;
        // abs(): a caller may pass a signed distance, and a negative one cast to uint32 wraps to a
        // multi-day stall instead of a transition.
        return static_cast<uint32>(static_cast<float>(baseMs) * std::fabs(dist) / reference);
    }

    // Glide a boost's carried speed buys: a fraction (BoostGlidePct) of the ordinary glide, so a full
    // BoostAdd of carry no longer pays the whole GlideMs and outlasts the next stack recharge. The
    // carry is capped at one BoostAdd, so stacks banked over a long climb cannot buy an unbounded
    // glide when they are finally released. Always called with the player's effective config.
    uint32 BoostCarryMs(const Config& c, float carry)
    {
        if (carry > c.boostAdd)
            carry = c.boostAdd;
        return ScaledMs(static_cast<uint32>(static_cast<float>(c.glideMs) * c.boostGlidePct),
                        carry, c.boostAdd);
    }
    // Every transition helper below reads the *effective* config of the player it is moving, never
    // g_cfg: the base value stays the ManaSpeedFlying.* key and a talent is a percentage of it, so
    // an update loop that read g_cfg would quietly ignore every rank a player owns.
    float RateFor(const Config& c, Motion m)
    {
        switch (m)
        {
            case Motion::Climb: return c.climbSpeed;
            case Motion::Dive:  return c.diveSpeed;
            default:            return c.levelSpeed; // Level and Hover
        }
    }

    float MaxRate(const Config& c) { return std::max(c.climbSpeed, std::max(c.levelSpeed, c.diveSpeed)); }
    float MinRate(const Config& c) { return std::min(c.climbSpeed, std::min(c.levelSpeed, c.diveSpeed)); }

    // Transition length for entering `next` from the state we are in.
    uint32 DurationFor(const Config& c, const PlayerState& st, Motion next)
    {
        const float nextRate = RateFor(c, next);
        // A hover (a parked mount) is a deliberately tiny accel/decel zone in both directions: the
        // speed drops to the parked level as the mount stops and comes straight back out when it
        // moves, so parking cannot bank a residual dive glide to spend on the next take-off.
        if (st.motion == Motion::Hover || next == Motion::Hover)
            return c.hoverMs;
        if (next == Motion::Dive)
            return c.diveMs;
        if (st.motion == Motion::Dive)
        {
            const float dist = std::fabs(nextRate - st.current);
            if (next == Motion::Climb)
                return ScaledMs(c.climbDecelMs, dist, std::fabs(c.diveSpeed - c.climbSpeed));
            if (next == Motion::Level)
            {
                // D4 Shallow Pull: a pull-out from a shallow dive keeps the whole glide instead of
                // only the slice the speed being shed would pay for. The pitch is tracked from the
                // dive's own position deltas, since the server never sees the client's camera.
                if (st.divePitch <= c.shallowPullDeg)
                    return c.glideMs;
                return ScaledMs(c.glideMs, dist, std::fabs(c.diveSpeed - c.levelSpeed));
            }
        }
        // A climb entered from a dive and cut short to level still carries the leftover dive speed,
        // so it glides out like a pull-out rather than blending like a fresh climb.
        if (st.motion == Motion::Climb && st.fromDive && next == Motion::Level)
        {
            const float dist = std::fabs(nextRate - st.current);
            if (st.divePitch <= c.shallowPullDeg)
                return c.glideMs;
            return ScaledMs(c.glideMs, dist, std::fabs(c.diveSpeed - c.levelSpeed));
        }
        // Everything else keeps the old span-proportional blend.
        const float span = MaxRate(c) - MinRate(c);
        if (span <= 0.0f)
            return 0;
        const float dist = std::fabs(nextRate - st.current);
        return static_cast<uint32>(static_cast<float>(c.blendMs) * dist / span);
    }

    // Curve used to accelerate a dive (a kick so it responds at once, then faster and faster),
    // hold a glide's speed before bleeding it off (ease-in), or neither.
    Curve CurveFor(const PlayerState& st, Motion next)
    {
        if (next == Motion::Dive)
            return Curve::Dive;
        // Ease-in holds the built-up speed first, then brakes hard near the end. Used for every pull
        // out of a dive: level (the long glide) and climb (hold the speed, then lose it).
        if (st.motion == Motion::Dive)
            return Curve::EaseIn;
        if (st.motion == Motion::Climb && st.fromDive && next == Motion::Level)
            return Curve::EaseIn;
        return Curve::Linear;
    }

    // The button boost: spends a stack and adds BoostAdd to the speed right now as an offset on top
    // of whatever blend is running. The offset fades over BoostFadeMs (fast at first, then slow).
    // BoostCarryPct of the added speed is not part of that offset: it is folded into the running
    // transition (or banked until a climb levels out) instead, so it bleeds off over the ordinary
    // glide and the boost still pays speed seconds after the burst is gone. There is no cooldown, so
    // held stacks can be chained. D5 Aftershock is not a second activation: it folds the live burst
    // into the running transition (see CommitBoost), so releasing the key spends no further stack.
    void ActivateBoost(PlayerState& st, const Config& c)
    {
        if (!st.eligible || st.stacks <= 0)
            return;
        // Usable while climbing, diving or gliding level. Only a hover (a parked mount) is left out,
        // so a boost cannot be spent to bank speed before a take-off.
        if (st.motion != Motion::Dive && st.motion != Motion::Level && st.motion != Motion::Climb)
            return;

        const float applied = st.current + st.boostOffset;
        const float boosted = std::min(c.boostSpeed, applied + c.boostAdd);
        if (boosted <= applied)
            return; // already at the ceiling

        st.stacks--;

        const float add = boosted - applied;
        float carry     = add * c.boostCarryPct;

        // A boost may raise the *base* glide only up to the fastest normal state (the dive ceiling);
        // speed past that stays in the fading burst offset instead. Without this, a boost spent at or
        // near the dive ceiling pushed the base above it, and a pull-out glide is scaled by the speed
        // being shed, so that one boost stretched the whole following glide -- the HUD's boost band
        // stayed lit for tens of seconds and the boost read as never emptying. Keeping the overrun in
        // the short burst means it fades on BoostFadeMs, not on the ordinary glide.
        const float room = std::max(0.0f, MaxRate(c) - st.current);
        if (carry > room)
            carry = room;

        // Replenish part of the boost as ordinary glide time. A climb is not a glide, so its carry
        // is banked: folding it into the running climb would hold speed past the climb. SetTarget
        // releases the banked carry when the climb ends -- on leveling out it rides its own glide, so
        // the retained half becomes glide time instead of emptying in a few seconds.
        if (carry > 0.0f && st.motion == Motion::Climb)
        {
            st.pendingCarry += carry;
        }
        // Otherwise raise the base transition and extend it only by what the carried speed itself
        // needs to shed -- a fraction of the ordinary glide -- instead of restarting the long
        // pull-out curve, so a boost lands back on the glide it had rather than outlasting the next
        // stack. The running transition keeps its own elapsed time and remaining span, so a boost
        // taken mid-glide does not reset the ease-in to the part that holds speed longest. Only fold
        // it when the base is decelerating (current at or above target); while a dive is still
        // accelerating, folding it would stall the climb to DiveSpeed, so the whole boost stays in
        // the burst offset instead.
        else if (carry > 0.0f && st.current >= st.target)
        {
            const uint32 left = st.elapsed < st.duration ? st.duration - st.elapsed : 0;
            st.start    = st.current + carry;
            st.current  = st.start;
            st.target   = RateFor(c, st.motion);
            st.duration = st.elapsed + left + BoostCarryMs(c, carry);
            st.curve    = Curve::EaseIn;
        }
        else
            carry = 0.0f;

        // Add to any boost still fading so chained stacks build one bigger burst instead of replacing.
        st.boostStart    = st.boostOffset + (add - carry);
        st.boostOffset   = st.boostStart;
        st.boostElapsed  = 0;
        st.boostDuration = c.boostFadeMs;
    }

    // D5 Aftershock: the boost key came up while the burst is still fading, so the burst is committed
    // to the running transition now instead of bleeding off over BoostFadeMs. No stack is spent -- the
    // key press that started the boost already paid for it -- so a diver whose stock is empty can
    // still land the burst it is riding. The transition is restarted from the committed rate because
    // folding into `current` alone is not durable: the update tick recomputes `current` from
    // start/target/elapsed, and would erase the commit on the very next frame.
    void CommitBoost(PlayerState& st, const Config& c)
    {
        if (st.boostOffset <= 0.0f)
            return; // the fade is already empty: nothing to commit, and restarting the curve would hurt
        st.current       = std::min(c.boostSpeed, st.current + st.boostOffset);
        st.start         = st.current;
        st.target        = RateFor(c, st.motion);
        st.elapsed       = 0;
        // A full dive accel is too long for a nudge that is already at the dive ceiling, so the
        // handoff keeps the usual span-proportional blend back down to the state rate.
        st.duration      = ScaledMs(c.diveMs, st.start - st.target, std::fabs(c.diveSpeed - c.climbSpeed));
        st.curve         = Curve::Dive;
        st.boostStart    = 0.0f;
        st.boostOffset   = 0.0f;
        st.boostElapsed  = 0;
        st.boostDuration = 0;
    }

    // Records the state a link message named. The first message on a mount snaps the applied rate so
    // a take-off does not spend seconds ramping from the mount's own speed.
    void SetTarget(PlayerState& st, const Config& c, Motion m)
    {
        st.sinceLink = 0;

        if (!st.eligible)
        {
            st.eligible  = true;
            st.motion    = m;
            st.current   = RateFor(c, m);
            st.start     = st.current;
            st.target    = st.current;
            st.elapsed   = 0;
            st.duration  = 0;
            st.curve     = Curve::Linear;
            st.fromDive  = false;
            st.settling  = false;
            st.settleAccum = 0;
            st.pendingCarry = 0.0f;
            st.lastSent  = -1.0f;
            st.sendAccum = c.sendMs; // force the first packet out on the next update
            return;
        }

        if (m == st.motion)
        {
            st.target = RateFor(c, m); // heartbeat: keep the running transition going
            return;
        }
        // A real state change: start a fresh transition from wherever the speed is now. It also
        // cancels a pending Skyward settle -- the state the player just chose outranks the elevated
        // level-out, and a settle left armed would pull the new transition back to levelSpeed.
        st.settling    = false;
        st.settleAccum = 0;
        const Motion prev = st.motion;
        if (m == Motion::Climb)
            st.fromDive = (prev == Motion::Dive);

        // Leaving a climb releases the boost carry that was banked there (a climb is not a glide, so
        // it had nowhere to fold). Leveling out merges the still-fading burst into the base first, so
        // the applied speed does not step up at the switch, and spends the carry on glide time (the
        // longer transition below) instead of adding it on top -- the bar glides out instead of
        // bumping. Any other change out of the climb just carries it into the new transition.
        float foldedCarry = 0.0f;
        if (prev == Motion::Climb && st.pendingCarry > 0.0f)
        {
            if (m == Motion::Level)
            {
                // Merge the fading burst into the base so the transition starts from the live speed
                // (no upward step), but only up to the fastest normal state (the dive ceiling):
                // anything past that is speed no plain state reaches, so it stays a fading burst
                // instead of raising the base and stretching the glide this level-out runs.
                const float live = std::min(c.boostSpeed, st.current + st.boostOffset);
                const float base = std::min(live, std::max(MaxRate(c), st.current));
                st.current       = base;
                st.boostStart    = live - base;
                st.boostOffset   = live - base;
                st.boostElapsed  = 0;
                st.boostDuration = st.boostStart > 0.0f ? c.boostFadeMs : 0;
                foldedCarry      = std::min(st.pendingCarry, std::max(0.0f, c.boostSpeed - base));
            }
            else
            {
                // A pull-out that still carries dive speed keeps its higher current speed; everything
                // else keeps the banked half folded into the new transition.
                const float base = st.current;
                foldedCarry = std::min(st.pendingCarry,
                                       std::max(0.0f, std::max(MaxRate(c), base) - base));
                st.current = base + foldedCarry;
            }
            st.pendingCarry = 0.0f;
        }

        st.start    = st.current;
        st.target   = RateFor(c, m);
        // G10 Skyward Path: a level-out ends above the level bar on the elevated rate, and the update
        // path then waits out SettleMs before dropping the rest of the way. Gated on coming *down* from
        // a dive or a climb: leveling out of a hover must not lift the mount above the bar.
        if (m == Motion::Level && c.levelElev > 0.0f && st.target < c.levelElev &&
            (prev == Motion::Dive || prev == Motion::Climb))
        {
            st.target     = c.levelElev;
            st.settleFrom = st.target;
            st.settling   = true;
        }
        st.elapsed  = 0;
        st.duration = DurationFor(c, st, m);
        st.curve    = CurveFor(st, m);
        // A banked carry pays for its own glide: a full BoostAdd of carry buys BoostGlidePct of the
        // ordinary GlideMs, a smaller carry proportionally less, so the retained speed is not dumped
        // in three or four seconds. Only when the mount is actually shedding speed -- an eased rise
        // from below the level speed would just delay settling, not carry momentum. Keep whichever
        // glide is longer, so a pull-out that still carries dive speed is never shortened.
        if (m == Motion::Level && foldedCarry > 0.0f && c.boostAdd > 0.0f && st.start > st.target)
        {
            const uint32 carryDur = BoostCarryMs(c, foldedCarry);
            if (carryDur > st.duration)
            {
                st.duration = carryDur;
                st.curve    = Curve::EaseIn;
            }
        }
        st.motion   = m;
        // Only a state change arms or clears a settle; the block above set the flag it needs, and this
        // line must not wipe it. The old settle was already cleared on entry to the !eligible branch.
    }

    // Publish the configured speeds and the live stack count to the client so its bar, particles,
    // shake and boost button scale to the values actually in play and its stack count cannot drift from
    // the server's. Same "WXLMS" prefix the client already sends, body
    // "S:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,<stacks>,<stackMs>,
    // <stackDiveMs>,<stackAccumMs>,<stackIntervalMs>"; the client polls
    // its last addon message, ignores a body it cannot parse, and uses the last two fields to anchor
    // the orb's recharge bar to the server's exact position (an older server omits them). A UI reload
    // on the client drops the value, which the slow resend covers.
    void SendSpeedAnnounce(Player* player, const Config& c, int stacks, uint32 stackAccum, uint32 stackInterval)
    {
        char body[160];
        std::snprintf(body, sizeof(body), "S:%.4g,%.4g,%.4g,%u,%d,%u,%d,%u,%u,%u,%u",
                      c.levelSpeed * 100.0f, c.diveSpeed * 100.0f, c.boostSpeed * 100.0f, 0u,
                      c.boostStacks, c.boostFadeMs, stacks, c.stackMs, c.stackDiveMs,
                      stackAccum, stackInterval);

        // The client listens for the addon prefix on CHAT_MSG_ADDON, which the client's chat layer only
        // parses out of a LANG_ADDON chat packet -- so the prefix rides at the front, exactly as a
        // client-sent SendAddonMessage lays it out.
        WorldPacket data;
        std::string message = std::string("WXLMS\t") + body;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->SendDirectMessage(&data);
    }

    // --- the talent tree --------------------------------------------------------------------------
    // Every one of these runs with g_lock held, so a rank write and the tally that answers it cannot
    // interleave with another message from the same client.

    // D5 Aftershock: the client's boost key was released while a boost is still fading. Only a diver
    // that owns the talent gets to fold the burst into a dive; for anyone else the fade runs its course.
    // The check is on the rank, not on the message, so a forged 'X' buys nothing.
    void HandleAftershock(uint64 guid)
    {
        auto pit = g_progress.find(guid);
        if (pit == g_progress.end() || !pit->second.loaded)
            return;
        const Talents& tree = pit->second.tree;
        for (int i = 0; i < kTalentCount; ++i)
            if (kTalents[i].id == 5 && !tree.ranks[i])
                return; // no rank in Aftershock

        auto it = g_states.find(guid);
        if (it == g_states.end())
            return;
        PlayerState& st = it->second;
        if (!st.eligible || !st.boostDuration || st.motion != Motion::Dive)
            return; // only a live boost released straight into a dive

        CommitBoost(st, pit->second.eff);
        st.sendAccum = pit->second.eff.sendMs; // push the folded speed out at once
    }

    // Sends one "WXLTT" body. Same routing as the speed announce: the prefix rides at the front of a
    // LANG_ADDON whisper, which is the only prefix the client's chat layer parses.
    void SendTalentLine(Player* player, const std::string& body)
    {
        WorldPacket data;
        std::string message = std::string("WXLTT\t") + body;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->SendDirectMessage(&data);
    }

    // The progress bar on its own: the partial point as a permille of the point being paid for, then
    // the yards that point still needs. This is the body that used to be the tally's tail, split out
    // so a bar tick does not drag every rank pair along with it -- the tail is on a ~500 ms cadence
    // for as long as the mount is banking yards, and the pairs are the bulk of a tally.
    void SendTalentBar(Player* player, uint32 permille, double need)
    {
        char body[64];
        std::snprintf(body, sizeof(body), "P:%u,%.0f", permille, need);
        SendTalentLine(player, body);
    }

    // The authoritative tally: lifetime points, points spent and unspent, then one id:rank pair per
    // talent the character owns, then the partial point as a permille and the yards the next point
    // still needs. Both tail fields are appended *after* the pairs on purpose: a client that scans the
    // pairs breaks out on a field it cannot read, so it keeps every rank and only loses the bar.
    // Only a rank or a point count can change here -- the bar moves on its own and rides SendTalentBar.
    void SendTalentTally(Player* player, Progress& p)
    {
        char body[512];
        int n = std::snprintf(body, sizeof(body), "T:%u,%d,%d", p.points, p.Spent(), p.Unspent());
        if (n < 0)
            return;
        for (int i = 0; i < kTalentCount && n < static_cast<int>(sizeof(body)) - 32; ++i)
        {
            if (p.tree.ranks[i] <= 0)
                continue;
            n += std::snprintf(body + n, sizeof(body) - static_cast<size_t>(n), ",%u:%d",
                               static_cast<unsigned>(kTalents[i].id), p.tree.ranks[i]);
            if (n < 0)
                return; // an encoding error is not a buffer length
        }
        if (n < static_cast<int>(sizeof(body)) - 32)
        {
            const double need = p.points < g_talentCfg.maxPoints ? PointYards(p.points, p.tree) : 0.0;
            p.shownPartial = PartialPermille(p.points, p.yardPartial, p.tree);
            std::snprintf(body + n, sizeof(body) - static_cast<size_t>(n), ",P%u,N%.0f",
                          p.shownPartial, need);
        }
        // The bar has just been drawn from this body, so record it: without this the next tick would
        // see a point count it has not shown and answer a tally that only repeats what we just sent.
        p.shownPoints = p.points;
        SendTalentLine(player, body);
    }

    // A refusal is answered, never dropped, so the panel cannot drift out of step with the server.
    void SendTalentRefusal(Player* player, const char* reason)
    {
        SendTalentLine(player, std::string("F:") + reason);
    }

    // Every write validates in this order: a loaded progress row, the id, the rank ceiling, the
    // tier's pick, the tiers above it, the lifetime cap, then the derived unspent count -- which is
    // what makes a forged message cost exactly what a real one would.
    void HandleTalentLearn(Player* player, uint64 guid, int talentId)
    {
        auto it = g_progress.find(guid);
        if (it == g_progress.end() || !it->second.loaded)
        {
            SendTalentRefusal(player, "no-progress");
            return;
        }
        Progress& p = it->second;

        int index = -1;
        for (int i = 0; i < kTalentCount; ++i)
            if (kTalents[i].id == talentId)
                index = i;
        if (index < 0)
        {
            SendTalentRefusal(player, "unknown-talent");
            return;
        }

        const TalentDef& t = kTalents[index];
        if (p.tree.ranks[index] + 1 > static_cast<int>(t.maxRank))
        {
            SendTalentRefusal(player, "max-rank");
            return;
        }
        // Only one talent of a (branch, tier) group may hold a rank: this is the tier-2 pick. An empty
        // rival is not a refusal, it is an ordinary investment; a ranked rival needs a refund first.
        // A bridge is exempt: it is the one talent of its tier that does not compete with the branch's
        // own pick, so Still Air and Steep Descent can both be ranked.
        if (!IsBridge(index))
        {
            for (int i = 0; i < kTalentCount; ++i)
                if (i != index && !IsBridge(i) && kTalents[i].branch == t.branch &&
                    kTalents[i].tier == t.tier && p.tree.ranks[i] > 0)
                {
                    SendTalentRefusal(player, "tier-taken");
                    return;
                }
        }
        // The bridge beside a branch opens its tiers 3 and 4 only. Tiers 1 and 2 are walkable without
        // it, so a player can start a branch and only then decide to bridge into it; before this the
        // whole branch was gated, which made the panel's tier-1 nodes refuse. Dive is the entry branch
        // and is always open, so its own gate never fires. Refused rather than dropped, so the panel
        // shows why the tier went grey.
        if (!IsBridge(index) && t.tier >= 3 && !p.tree.BranchOpen(t.branch))
        {
            SendTalentRefusal(player, "tree-locked");
            return;
        }
        // A bridge bypasses the tier rules, not the branch it sits in: 17 lives in Glide, so Wings
        // cannot be opened straight from Dive -- 16 must be bought first. Dive is the entry branch, so
        // 16 itself passes. The client applies the same rule when it decides a node is clickable.
        if (IsBridge(index) && !p.tree.BranchOpen(t.branch))
        {
            SendTalentRefusal(player, "tree-locked");
            return;
        }
        // A bridge has no tier above it to unlock: it is the way in, not a step up.
        if (!IsBridge(index))
            for (uint8 r = 1; r < t.tier; ++r)
                if (!p.tree.TierDone(t.branch, r))
                {
                    SendTalentRefusal(player, "tier-locked");
                    return;
                }
        // Only unspent points can be spent -- never the lifetime count, or a character that earned
        // the cap and then respecced could never spend the points the refund gave back.
        if (p.Unspent() <= 0)
        {
            SendTalentRefusal(player, "no-points");
            return;
        }

        p.tree.ranks[index]++;
        p.Rebuild(g_cfg, g_speedCapPct);
        p.dirty = true;
        LOG_INFO("modules", "mod-mana-speedflying: {} learnt talent {} (rank {}/{})",
                 guid, t.id, p.tree.ranks[index], t.maxRank);
        SendTalentTally(player, p);
    }

    // Respec: refund one branch ("b1".."b3", or a legacy "1".."3") or the whole tree ("A"). Free
    // unless Talents.RespecCost is set. A branch is the only scope that can be refunded on its own: a
    // single tier cannot, because a tier below it only opens on it being full, and refunding one talent
    // of a tier-2 pick would leave the group half-picked. The argument is parsed strictly -- a malformed
    // one is refused, never defaulted, because "refund everything" is the one destructive path.
    void HandleTalentRefund(Player* player, uint64 guid, const std::string& what)
    {
        auto it = g_progress.find(guid);
        if (it == g_progress.end() || !it->second.loaded)
        {
            SendTalentRefusal(player, "no-progress");
            return;
        }
        Progress& p = it->second;

        int branch = -1; // -1 = every branch
        if (what == "A")
        {
            branch = -1;
        }
        else
        {
            // "b<N>" is what this client sends; a bare "<N>" is accepted so an older build of it keeps
            // working. Anything else -- empty, zero, non-numeric, out of range -- is refused.
            const char* digits = (what.size() >= 2 && what[0] == 'b') ? what.c_str() + 1 : what.c_str();
            if (digits[0] < '1' || digits[0] > '3' || digits[1] != '\0')
            {
                SendTalentRefusal(player, "bad-branch");
                return;
            }
            branch = digits[0] - '0' - 1;
        }

        // Check affordability once, before anything is refunded, so a half-applied respec is not a
        // state this module can be in.
        if (g_talentCfg.respecCost && !player->HasEnoughMoney(g_talentCfg.respecCost))
        {
            SendTalentRefusal(player, "no-money");
            return;
        }

        int refunded = 0;
        for (int i = 0; i < kTalentCount; ++i)
        {
            if (branch >= 0 && kTalents[i].branch != branch)
                continue;
            if (!p.tree.ranks[i])
                continue;
            p.tree.ranks[i] = 0;
            ++refunded;
        }
        if (!refunded)
        {
            SendTalentTally(player, p); // nothing to refund; still answer, so the panel redraws
            return;
        }

        if (g_talentCfg.respecCost)
            player->ModifyMoney(-static_cast<int32>(g_talentCfg.respecCost));
        p.Rebuild(g_cfg, g_speedCapPct);
        p.dirty = true;
        SendTalentTally(player, p);
    }

    // Parses a "WXLTT" body. Everything arriving here is a *request*: the server decides. A refusal is
    // an answer, never a silent drop, so the client panel cannot drift out of step.
    //   WXLTT\tW\t<talentId>   WXLTT\tR\t<b1|b2|b3|A>   WXLTT\tQ
    void HandleTalentMessage(Player* player, const std::string& msg)
    {
        const size_t op = 6;
        if (msg.size() <= op)
            return;

        // The 'Q' tally request only reads and is answered on foot too, so the mounted gate below
        // covers W and R alone -- an unmounted player still gets the panel's refresh.
        const uint64 guid = player->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> guard(g_lock);

        if (!g_talentCfg.enabled)
        {
            SendTalentRefusal(player, "disabled");
            return;
        }

        // Only mounted flight earns and only a mount spends: a respec while riding a ground mount is
        // still fine, but an on-foot character cannot touch the tree.
        if (msg[op] != 'Q' && !player->IsMounted())
        {
            SendTalentRefusal(player, "not-mounted");
            return;
        }

        switch (msg[op])
        {
            case 'W':
            {
                // A truncated argument, a missing separator or a number too large for an int would
                // otherwise be parsed out of whatever follows; the channel is untrusted input.
                if (msg.size() <= op + 2 || msg[op + 1] != '\t')
                {
                    SendTalentRefusal(player, "bad-arg");
                    break;
                }
                const char* arg = msg.c_str() + op + 2;
                char*       end = nullptr;
                const long  id  = std::strtol(arg, &end, 10);
                if (end == arg || *end != '\0' || id < std::numeric_limits<int>::min() ||
                    id > std::numeric_limits<int>::max())
                {
                    SendTalentRefusal(player, "bad-arg");
                    break;
                }
                HandleTalentLearn(player, guid, static_cast<int>(id));
                break;
            }
            case 'R':
                if (msg.size() <= op + 1)
                {
                    SendTalentRefusal(player, "bad-arg");
                    break;
                }
                HandleTalentRefund(player, guid, msg.substr(op + 2));
                break;
            case 'Q':
            {
                auto it = g_progress.find(guid);
                if (it == g_progress.end() || !it->second.loaded)
                    SendTalentRefusal(player, "no-progress");
                else
                    SendTalentTally(player, it->second);
                break;
            }
            default:
                break; // an unknown op is a newer client's; leave it for a build that knows it
        }
    }

class ManaSpeedFlyingPlayer : public PlayerScript
{
public:
    ManaSpeedFlyingPlayer()
        : PlayerScript("ManaSpeedFlyingPlayer",
                       { PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGIN,
                         PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_DELETE }) {}

    // The client link. wxl-mana-speedflying senders are the only ones that matter; everything else on the
    // addon channel is left as the core received it. The channel is untrusted: a '1' link is only
    // applied to a flying player and a boost off the ground is dropped, but the server cannot tell a
    // real client from a forged message -- it only ever sets a flight speed.
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& /*type*/, uint32& lang, std::string& msg) override
    {
        if (!player || lang != LANG_ADDON)
            return;

        // The talent tree rides its own prefix, so the link parser below is left exactly as it was.
        if (msg.size() >= 6 && msg.compare(0, 6, "WXLTT\t") == 0)
        {
            HandleTalentMessage(player, msg);
            return;
        }

        if (msg.size() < 8 || msg.compare(0, 6, "WXLMS\t") != 0)
            return;

        const char advfly = msg[6];
        const char state  = msg[7];

        bool announce = false;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (!g_cfg.enabled)
                return;

            const uint64 guid = player->GetGUID().GetRawValue();
            if (advfly != '1')
            {
                // Not an AdvFly mount (or no mount): stand down without minting a state entry. The
                // update path restores the core's own speed for a link that was up.
                auto it = g_states.find(guid);
                if (it != g_states.end())
                {
                    it->second.eligible  = false;
                    it->second.announced = false;
                }
                return;
            }

            PlayerState& st = g_states[guid];

            const uint32 now = getMSTime();
            // A client sends at most one message per ~200 ms; anything faster is a flood. Boost
            // messages are never throttled (each spends a stack and must be answered), so the
            // client's stack count cannot desync from a dropped one.
            // The flood guard exempts 'B' because each boost spends a stack and must be answered.
            if (state != 'B' && state != 'X' && st.lastMsgMs && now - st.lastMsgMs < 50)
                return;
            st.lastMsgMs = now;

            // First contact gets the server's speeds at once; after that a slow resend covers a client
            // UI reload (which wipes the value) without chattering on every state change.
            if (!st.announced || now - st.lastAnnounce >= 5000)
            {
                st.announced    = true;
                st.lastAnnounce = now;
                announce        = true;
            }

            // The button replenishes the glide: it lifts the speed and starts a fresh glide, so the
            // added speed is spent as extra glide time instead of a fixed timed burst. The server only
            // drives a flying mount, so a boost off the ground is answered (to keep the client's count
            // honest) but not applied.
            // Everything below reads this player's own effective config, so a talent actually moves
            // the speed instead of only being validated.
            const Config eff = EffectiveConfig(guid);

            if (state == 'B')
            {
                st.sinceLink = 0;
                if (player->IsFlying())
                    ActivateBoost(st, eff);
                // Answer every attempt with the authoritative count, spent or rejected, so a click the
                // server ignored cannot leave the client button one stack off until the slow resend.
                st.lastAnnounce = now;
                announce        = true;
            }
            else if (state == 'X')
            {
                // D5 Aftershock: the boost key came up while a boost is still live. Handled inline
                // (not via SetTarget) because it must run even when the state letter is unchanged.
                st.sinceLink = 0;
                HandleAftershock(guid);
                st.lastAnnounce = now;
                announce        = true;
            }
            else
            {
                switch (state)
                {
                    case 'C': SetTarget(st, eff, Motion::Climb); break;
                    case 'D': SetTarget(st, eff, Motion::Dive);  break;
                    case 'H': SetTarget(st, eff, Motion::Hover); break;
                    default:  SetTarget(st, eff, Motion::Level); break;
                }
            }
            if (announce)
                SendSpeedAnnounce(player, eff, st.stacks, st.stackAccum,
                                  st.motion == Motion::Dive ? eff.stackDiveMs : eff.stackMs);
        }
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        if (!player || player->IsInFlight() || player->GetVehicle())
            return;

        // A lagging or suspended server (a local worldserver resumed from sleep, say) can hand us a
        // multi-second p_time. Trusting it lets the recharge block below dump the whole boost stock --
        // and snap every transition -- in one tick. Bound it to a sane frame.
        p_time = std::min<uint32>(p_time, 1000);

        const uint64 guid   = player->GetGUID().GetRawValue();
        const bool   flying = player->IsFlying();

        // The whole read-modify-write runs under one lock: the chat hook can interleave a SetTarget or
        // a boost, and a second lock later would let that update be overwritten by this tick's stale
        // write-back. All config reads are inside this lock too, so a reload cannot tear them.
        std::lock_guard<std::mutex> guard(g_lock);

        // The progress row is read once, on login, outside this lock (see OnPlayerLogin). A player whose
        // row is somehow missing is treated as a fresh character rather than queried here: a blocking
        // database read under g_lock would stall every other player's link tick on this thread.
        auto pit = g_progress.find(guid);
        if (pit == g_progress.end())
        {
            // A bare Progress default-constructs eff from the Config member initializers, not from the
            // INI, so a tick arriving before OnPlayerLogin would silently drive the mount at the
            // shipped defaults. Build the placeholder from the live base instead.
            Progress fresh;
            fresh.eff = g_cfg;
            ApplyTalents(fresh.eff, fresh.tree, g_speedCapPct);
            pit = g_progress.emplace(guid, std::move(fresh)).first;
        }
        Progress& progress = pit->second;

        // Everything the update tick applies comes from the player's own effective config.
        const Config eff = EffectiveConfig(guid);

        // Talent points are earned here too. A relocated first position is adopted rather than paid,
        // so a login or a teleport cannot mint a point.
        if (g_talentCfg.enabled)
        {
            const bool earning = flying && player->IsMounted() && player->GetMountID() &&
                                 g_states.count(guid) && g_states[guid].eligible;
            const float x = player->GetPositionX();
            const float y = player->GetPositionY();
            const float z = player->GetPositionZ();
            if (earning)
            {
                if (progress.havePos)
                {
                    const float dx = x - progress.lastX;
                    const float dy = y - progress.lastY;
                    const float dz = z - progress.lastZ;
                    const float yards = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (yards < g_talentCfg.teleportYards)
                    {
                        // Diving pays best: the same yard counts more while heading down.
                        const Motion m = g_states[guid].motion;
                        // A dive's own slope, for D4: the server never sees which way the client looks.
                        if (m == Motion::Dive && yards > 1.0f)
                            g_states[guid].divePitch = static_cast<float>(
                                std::atan2(-dz, std::sqrt(dx * dx + dy * dy)) * 57.2957795);
                        const float weight = (m == Motion::Dive) ? g_talentCfg.diveAccrual : 1.0f;
                        progress.yardPartial += static_cast<double>(yards * weight);
                        if (progress.yardPartial > 0.0)
                            progress.dirty = true; // the fraction survives a logout too
                        // The award cascade: one tick can cross several (increasingly longer) steps of the
                        // curve after a teleport-free burst of speed, so the loop re-reads the yard cost
                        // of the point it is about to pay. That cost is also the meter the panel reads as
                        // pointIndex/maxPoints, so no second counter has to be kept in step with it.
                        while (progress.yardPartial >= 1.0)
                        {
                            if (progress.points >= g_talentCfg.maxPoints)
                            {
                                progress.yardPartial = 0.0; // the tree is full: stop banking yards
                                break;
                            }
                            if (g_talentCfg.maxPointsPerMinute &&
                                progress.recentMs.size() >= g_talentCfg.maxPointsPerMinute)
                                break; // the rolling cap: an AFK orbit is not the whole economy
                            const double need = PointYards(progress.points, progress.tree);
                            if (progress.yardPartial < need)
                                break;
                            progress.yardPartial -= need;
                            progress.points++;
                            progress.recentMs.push_back(0);
                            progress.dirty = true;
                            LOG_INFO("modules", "mod-mana-speedflying: {} earned talent point {}",
                                     guid, progress.points);
                        }
                    }
                }
            }
            progress.lastX = x;
            progress.lastY = y;
            progress.lastZ = z;
            progress.havePos = earning;
            // The rolling minute: age every sample that paid a point and drop the ones that have
            // just left the window, so an unbroken flight keeps earning MaxPointsPerMinute a minute.
            for (uint32& age : progress.recentMs)
                age += p_time;
            while (!progress.recentMs.empty() && progress.recentMs.front() >= 60000)
                progress.recentMs.erase(progress.recentMs.begin());
        }

        auto it = g_states.find(guid);
        if (it == g_states.end())
            return;
        PlayerState& st = it->second;

        // Advance the link clock even while grounded, so a link that dies as the mount lands still
        // times out instead of freezing eligible and resuming stale on the next take-off.
        if (st.eligible && eff.timeoutMs)
        {
            st.sinceLink += p_time;
            if (st.sinceLink > eff.timeoutMs)
            {
                st.eligible  = false; // link went quiet: the client is gone, stop driving the mount
                st.announced = false; // force the speeds onto the next link
            }
        }

        if (!eff.enabled || !flying || !st.eligible)
        {
            // Restore the mount's own speed once and clear the running blend and boost, so a grounded,
            // unlinked or switched-off mount cannot resume a stale rate on the next take-off. Banked
            // boost stacks are kept: a grounded mount earns none, but does not lose the ones it built.
            if (st.eligible)
            {
                st.eligible  = false;
                st.announced = false;
            }
            if (st.lastSent >= 0.0f)
            {
                player->UpdateSpeed(MOVE_FLIGHT, true);
                st.lastSent = -1.0f;
            }
            st.current       = 0.0f;
            st.target        = 0.0f;
            st.start         = 0.0f;
            st.elapsed       = 0;
            st.duration      = 0;
            st.fromDive      = false;
            st.sendAccum     = 0;
            st.stackAccum    = 0; // grounded earns nothing: do not resume half a stack on take-off
            st.stackInterval = 0;
            st.boostStart    = 0.0f;
            st.boostOffset   = 0.0f;
            st.boostElapsed  = 0;
            st.boostDuration = 0;
            st.pendingCarry  = 0.0f;
            st.settling      = false; // grounded: a pending Skyward settle must not resume on take-off
            st.settleAccum   = 0;
            return;
        }

        // Eased transition from `start` to `target` over `duration`. This keeps running under a
        // boost, so the boost is only an offset added on top of it below.
        if (st.duration == 0)
        {
            st.current = st.target;
        }
        else
        {
            st.elapsed += p_time;
            if (st.elapsed >= st.duration)
            {
                st.elapsed = st.duration;
                st.current = st.target;
            }
            else
            {
                const float p = static_cast<float>(st.elapsed) / static_cast<float>(st.duration);
                float e = p;
                if (st.curve == Curve::EaseIn)
                    e = p * p;
                else if (st.curve == Curve::Dive)
                    e = p * (0.35f + 0.65f * p); // kick off the line, then keep accelerating
                st.current = st.start + (st.target - st.start) * e;
            }
        }

        // G10's second phase: once the glide has reached the elevated rate, the mount drops to the plain
        // level speed over a fixed settle. Gated on the transition above having finished, or the settle
        // would race the very glide it is supposed to follow.
        if (st.settling && st.elapsed >= st.duration)
        {
            st.settleAccum += p_time;
            if (st.settleAccum >= eff.settleMs)
            {
                st.current     = eff.levelSpeed;
                st.settling    = false;
                st.settleAccum = 0;
            }
            else
                st.current = st.settleFrom +
                             (eff.levelSpeed - st.settleFrom) *
                                 (static_cast<float>(st.settleAccum) / static_cast<float>(eff.settleMs));
            // The settle owns the applied rate while it runs: pin the target to it so the transition
            // block above cannot pull the speed back to the elevated rate on the next tick.
            st.target = st.current;
        }

        // Boost stacks build on a timer: one every StackIntervalMs, or every StackDiveIntervalMs
        // while diving, capped at the configured stock. The timer only runs while the mount is
        // link-eligible, so a grounded or unlinked mount earns nothing.
        const int    prevStacks       = st.stacks;
        const uint32 stackIntervalNow = st.motion == Motion::Dive ? eff.stackDiveMs : eff.stackMs;
        if (st.stacks < eff.boostStacks)
        {
            if (stackIntervalNow)
            {
                // A cadence switch keeps the fraction of progress already made, not the raw
                // milliseconds: the bar holds its fill and the remaining work runs at the new rate.
                if (st.stackInterval && st.stackInterval != stackIntervalNow && st.stackAccum)
                    st.stackAccum = static_cast<uint32>(static_cast<uint64>(st.stackAccum) * stackIntervalNow /
                                                        st.stackInterval);
                st.stackAccum += p_time;
                if (st.stackAccum >= stackIntervalNow)
                {
                    st.stackAccum = 0; // one stack per update: a single long p_time must never fill the bar
                    ++st.stacks;
                }
            }
        }
        else
            st.stackAccum = 0;
        st.stackInterval = stackIntervalNow;

        // The boost is an additive offset on top of the running transition, fading to nothing over
        // BoostFadeMs: fast at first, then slow. Because the underlying transition keeps running, the
        // speed lands back on exactly the glide the mount had before the boost instead of snapping to
        // the plain state speed.
        if (st.boostDuration)
        {
            st.boostElapsed += p_time;
            if (st.boostElapsed >= st.boostDuration)
            {
                st.boostOffset   = 0.0f;
                st.boostElapsed  = 0;
                st.boostDuration = 0;
            }
            else
            {
                const float p    = static_cast<float>(st.boostElapsed) / static_cast<float>(st.boostDuration);
                const float keep = (1.0f - p) * (1.0f - p); // fast at first, then slow
                st.boostOffset = st.boostStart * keep;
            }
        }

        const float applied = std::min(eff.boostSpeed, st.current + st.boostOffset);
        // Settled once the base transition is done and any boost has fully faded, so the exact final
        // value is always pushed. Testing `current == target` instead would fire every tick during a
        // boost fade (the base is settled but the offset is not), defeating the SendMs throttle.
        const bool settled = (applied == st.target && st.boostDuration == 0);

        st.sendAccum += p_time;
        if (st.sendAccum >= eff.sendMs || settled)
        {
            st.sendAccum = 0;
            // Quantize before the compare: the blend moves by hundredths and a spline broadcast per
            // hundredth is pure churn. The client ramps between what it is told, so the wire value is
            // authority only -- a coarser step is invisible on the bar and much cheaper to broadcast.
            const float quantized = std::round(applied * 100.0f) * 0.01f;
            if (quantized != st.lastSent)
            {
                player->SetSpeed(MOVE_FLIGHT, quantized, true);
                st.lastSent = quantized;
            }
        }

        // A stack change is pushed at once, so the client button never lags the count the next boost
        // will actually spend. Only a point count or a rank pair needs the whole tally; the bar's own
        // movement rides the 'P' body below.
        if (st.stacks != prevStacks)
            SendSpeedAnnounce(player, eff, st.stacks, st.stackAccum, st.stackInterval);

        // Flush the earned points on a timer, not on every award: a point is a row write, not a
        // per-tick event, and a crash between flushes costs only the partial point in memory.
        if (g_talentCfg.flushMs && (progress.accMs += p_time) >= g_talentCfg.flushMs)
        {
            progress.accMs = 0;
            g_flushWanted = true; // the write rides the world tick, never this lock
        }

        // The talent bar's fill. The bar has no other way to see the partial point move, so it is
        // announced on a coarse floor when the permille changes -- but it is the bar alone, two fields,
        // not the tally's rank pairs. A tally is for a rank or a point, and both already send one.
        if ((progress.progMs += p_time) >= 500)
        {
            progress.progMs = 0;
            const uint32 partial = PartialPermille(progress.points, progress.yardPartial, progress.tree);
            if (progress.points != progress.shownPoints)
            {
                // A point was paid on this tick: the count, the unspent total and the next point's
                // cost all moved, so that one gets a tally. It is a rare event -- the caps allow a
                // handful a minute -- so the pairs cost nothing over the bar ticks between them.
                SendTalentTally(player, progress);
            }
            else if (partial != progress.shownPartial)
            {
                progress.shownPartial = partial;
                const double need = progress.points < g_talentCfg.maxPoints
                                        ? PointYards(progress.points, progress.tree)
                                        : 0.0;
                SendTalentBar(player, partial, need);
            }
        }
    }

    // Read the character's points and ranks once, on the session's own thread. The two queries block,
    // which is why this is not done in OnPlayerUpdate: that runs under g_lock and a read there would
    // stall every other player's link tick behind it.
    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;
        const uint64 guid = player->GetGUID().GetRawValue();
        // Copy the published config before releasing the lock: LoadProgress builds an effective config
        // from it and does its blocking queries outside the lock, on this session's own thread.
        Config base;
        float capPct = 1600.0f;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            base   = g_cfg;
            capPct = g_speedCapPct;
        }
        Progress p;
        LoadProgress(guid, base, capPct, p);

        std::lock_guard<std::mutex> guard(g_lock);
        // A reconnect can win the race against a slow logout; an entry already there keeps its ranks.
        // An entry the update path minted as a placeholder carries no row, so it is replaced -- only
        // marking it loaded would let the next write-back overwrite the stored tree with zeros.
        auto it = g_progress.find(guid);
        if (it == g_progress.end())
            g_progress.emplace(guid, std::move(p));
        else if (!it->second.loaded)
            it->second = std::move(p);
    }

    // The link state and the progress map both go on logout: the state is volatile, the progress is
    // flushed first so a relog finds exactly the ranks and the points it earned.
    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(g_lock);
        const uint64 guid = player->GetGUID().GetRawValue();
        auto pit = g_progress.find(guid);
        if (pit != g_progress.end())
        {
            if (pit->second.dirty)
                WriteProgress(guid, pit->second);
            g_progress.erase(pit);
        }
        g_states.erase(guid);
    }

    // Character deletion takes the talent rows with it, so a recycled guid cannot inherit a tree.
    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        const uint64 raw = guid.GetRawValue();
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_progress.erase(raw);
            g_states.erase(raw); // a recycled guid must not inherit the deleted one's live link state
        }
        CharacterDatabase.Execute("DELETE FROM `character_speedflying_talents` WHERE `guid` = {}",
                                   DbGuid(raw));
        CharacterDatabase.Execute("DELETE FROM `character_speedflying` WHERE `guid` = {}", DbGuid(raw));
    }
};

class ManaSpeedFlyingWorld : public WorldScript
{
public:
    ManaSpeedFlyingWorld()
        : WorldScript("ManaSpeedFlyingWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) {}

    // The timer flush, drained here rather than in OnPlayerUpdate: this tick holds no lock, so a slow
    // character database stalls only this write, not every other player's link tick behind g_lock.
    void OnUpdate(uint32 /*diff*/) override
    {
        if (g_flushWanted.exchange(false))
            FlushAllProgressUnlocked();
    }

    // The shutdown flush the review asked for: without it a graceful restart loses every rank bought
    // and every yard flown since the last flush. Runs while the maps are still alive, hence the hook.
    void OnShutdown() override
    {
        std::lock_guard<std::mutex> guard(g_lock);
        FlushAllProgress(true);
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        Config cfg;
        cfg.enabled      = sConfigMgr->GetOption<bool>("ManaSpeedFlying.Enable", true);
        cfg.climbSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.ClimbSpeed", 2.0f);
        cfg.levelSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.LevelSpeed", 2.8f);
        cfg.diveSpeed    = sConfigMgr->GetOption<float>("ManaSpeedFlying.DiveSpeed", 8.0f);
        cfg.blendMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BlendMs", 5500);
        cfg.diveMs       = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.DiveMs", 4000);
        cfg.glideMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.GlideMs", 18000);
        cfg.climbDecelMs = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.ClimbDecelMs", 3500);
        cfg.hoverMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.HoverMs", 500);
        cfg.sendMs       = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.SendMs", 100);
        cfg.timeoutMs    = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.TimeoutMs", 2500);
        cfg.boostAdd     = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostAdd", 4.0f);
        cfg.boostSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostSpeed", 16.0f);
        cfg.boostFadeMs  = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BoostFadeMs", 4000);
        cfg.boostCarryPct = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostCarryPct", 0.50f);
        cfg.boostGlidePct = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostGlidePct", 0.70f);
        cfg.boostStacks  = sConfigMgr->GetOption<int>("ManaSpeedFlying.BoostMaxStacks", 3);
        cfg.stackMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.StackIntervalMs", 9000);
        cfg.stackDiveMs  = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.StackDiveIntervalMs", 3000);

        // The four talent keys. Talents.Enable gates earning and spending; the rest default so the
        // module runs with no talent key present at all.
        TalentConfig tc;
        tc.enabled        = sConfigMgr->GetOption<bool>("Talents.Enable", true);
        // The earning curve: the first point costs Talents.BaseYards, each one after it costs
        // Talents.YardStep more than the point before (1.0 = twice as much).
        tc.baseYards      = sConfigMgr->GetOption<float>("Talents.BaseYards", 500.0f);
        tc.yardStep       = sConfigMgr->GetOption<float>("Talents.YardStep", 1.0f);
        // 17 talents now, so the panel lays out the cap to the largest tree it can actually draw.
        tc.maxPoints      = sConfigMgr->GetOption<uint32>("Talents.MaxPoints", 13);
        tc.maxPointsPerMinute = sConfigMgr->GetOption<uint32>("Talents.MaxPointsPerMinute", 4);
        tc.respecCost     = sConfigMgr->GetOption<uint32>("Talents.RespecCost", 0);
        tc.flushMs        = sConfigMgr->GetOption<uint32>("Talents.FlushMs", 20000);
        tc.teleportYards  = sConfigMgr->GetOption<float>("Talents.TeleportYards", 200.0f);
        tc.diveAccrual    = sConfigMgr->GetOption<float>("Talents.DiveAccrual", 1.5f);

        float speedCapPct = sConfigMgr->GetOption<float>("ManaSpeedFlying.SpeedCapPct", 1600.0f);
        speedCapPct = std::max(10.0f, speedCapPct);

        cfg.climbSpeed   = std::max(cfg.climbSpeed,   0.1f);
        cfg.levelSpeed   = std::max(cfg.levelSpeed,   0.1f);
        cfg.diveSpeed    = std::max(cfg.diveSpeed,    0.1f);
        cfg.boostAdd     = std::max(cfg.boostAdd,     0.0f);
        // The boost ceiling must sit at or above the fastest state speed, or the min() in the update
        // path would clamp ordinary (unboosted) level/climb flight below its configured rate.
        cfg.boostSpeed   = std::max(cfg.boostSpeed,
                                    std::max(cfg.climbSpeed, std::max(cfg.levelSpeed, cfg.diveSpeed)));
        cfg.boostStacks  = std::max(0, std::min(cfg.boostStacks, 3));
        cfg.boostCarryPct = std::max(0.0f, std::min(cfg.boostCarryPct, 1.0f));
        cfg.boostGlidePct = std::max(0.0f, std::min(cfg.boostGlidePct, 1.0f));
        if (!cfg.boostFadeMs) cfg.boostFadeMs = 1;
        if (!cfg.stackMs) cfg.stackMs = 1;
        if (!cfg.stackDiveMs) cfg.stackDiveMs = 1;
        if (!cfg.sendMs) cfg.sendMs = 1;
        if (tc.baseYards < 1.0f) tc.baseYards = 1.0f;
        // A negative or huge step would make the curve non-monotonic or overflow the double.
        tc.yardStep = std::max(0.0f, std::min(tc.yardStep, 10.0f));
        if (!tc.maxPoints) tc.maxPoints = 13;
        tc.maxPoints = std::min(tc.maxPoints, 64u); // 13 is the shipped default; this only guards a typo
        if (tc.teleportYards < 50.0f) tc.teleportYards = 50.0f;
        tc.diveAccrual = std::max(0.0f, tc.diveAccrual);
        // A cost above int32's range would flip the sign of the ModifyMoney argument below.
        if (tc.respecCost > static_cast<uint32>(std::numeric_limits<int32>::max()))
            tc.respecCost = static_cast<uint32>(std::numeric_limits<int32>::max());

        // Publish the new config and drop every link under the lock readers take. The entries are kept
        // (only marked ineligible) so the update path can restore each mount's own speed: clearing the
        // map would leave the last blended rate applied with nothing left to repair it.
        std::lock_guard<std::mutex> guard(g_lock);
        g_cfg = cfg;
        g_talentCfg = tc;
        g_speedCapPct = speedCapPct;

        // Every live player's effective config is rebuilt from the new base, so a reload reaches
        // ranks that were bought against the old values without a relog.
        for (auto& kv : g_progress)
        {
            kv.second.eff = cfg;
            ApplyTalents(kv.second.eff, kv.second.tree, speedCapPct);
        }

        for (auto& kv : g_states)
        {
            kv.second.eligible  = false;
            kv.second.announced = false;
        }
    }
};
} // namespace

void AddSC_ManaSpeedFlying()
{
    new ManaSpeedFlyingPlayer();
    new ManaSpeedFlyingWorld();
}
