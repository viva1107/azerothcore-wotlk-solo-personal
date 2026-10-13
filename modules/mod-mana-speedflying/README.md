# mod-mana-speedflying

An AzerothCore module that varies a flying mount's speed with how it is flying, as a server-side
companion to the client's **wxl-mana-speedflying** module:

| state      | speed |
|------------|-------|
| climbing   | 200%  |
| leveling   | 280%  |
| diving     | 800%  |

`wxl-mana-speedflying` already animates the mount as climbing / gliding / diving, so the two agree: wings
beat slowly on the way up, the glide rushes on the way down. The speed does not snap between states:

- **Diving** accelerates. The speed builds into `DiveSpeed` over `DiveMs`; it kicks off the line
  so the dive responds at once, then keeps accelerating as it goes.
- **Diving, then leveling** is a long glide over `GlideMs`, scaled by how much speed is actually
  being shed: pulling out of a shallow dive recovers sooner than a full `DiveSpeed` dive does.
  The mount keeps most of its dive speed at first and only bleeds it off hard near the end, before
  settling back to `LevelSpeed`.
- **Diving, then climbing** holds the dive speed for a moment over `ClimbDecelMs` (scaled the same
  way) so the built-up speed can be felt, then brakes hard into `ClimbSpeed`.
- **Diving, then climb, then level** (the pull-up is cut short) still carries the leftover dive
  speed: it glides out over `GlideMs` like a pull-out instead of blending like a fresh climb. A
  boost spent during that climb is banked and released onto that glide too, so the momentum is not
  lost.
- Leaving a **hover** (a parked mount) uses a fixed, tiny `HoverMs` zone whatever it heads into --
  climb, level or dive -- so a take-off responds at once instead of ramping from a standstill.
- Every other change blends over `BlendMs`, spread over how far the speed has to move.

## How it binds to the client module

The client module does the thinking and tells the server directly, over the addon channel (the only
channel a native client module has):

```
WXLMS<TAB><advfly:0|1><state:L|C|D|H|B>
```

- `advfly` is `1` only for mounts whose model carries the retail **AdvFly** clips -- exactly the
  mounts `wxl-mana-speedflying` drives with AdvFly. This is what keeps the speed change off stock mounts
  and druid flight forms, with no server-side mount list. A `0` message tells the server to restore
  the mount's own speed and stand down.
- `state` is `C` climb, `D` dive, `H` hovering, `L` level / strafe / backward, and `B` the
  client's button boost. `B` spends a stack and adds `BoostAdd` to the mount's current speed: a
  burst that fades over `BoostFadeMs`, plus a slice (`BoostCarryPct`) folded into the running
  glide so the speed is still carried seconds later. A climb or hover keeps its own transition.

The client sends on every state change, announces the mount's capability when a mount is loaded, and
heartbeats about once a second. On the server the message is picked up by
`PlayerScript::OnPlayerBeforeSendChatMessage` (addon messages, `LANG_ADDON`), and a periodic
`OnPlayerUpdate` blends `MOVE_FLIGHT` toward the state's speed.

### The server answers with its speeds

The client scales its speed bar, its speed particles and its dive shake to the top dive speed, so
it has to know what that speed is. On the same `WXLMS` prefix the server replies with its configured
values:

```
WXLMS<TAB>S:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,<stacks>,<stackMs>,<stackDiveMs>,<stackAccumMs>,<stackIntervalMs>
                                       e.g. WXLMS\tS:280,800,1600,0,3,4000,2,9000,3000,1800,9000
```

The client reads the last addon message a few times a second and uses `levelPct`/`divePct` as the
empty and full ends of every speed effect, with `boostPct` as the boost ceiling and the two stack
intervals as the recharge cadence. The trailing `stackAccumMs`/`stackIntervalMs` are how far the
server has already accrued toward the next stack and the cadence it accrued them at: the client
anchors the orb's recharge bar to that exact position on every announce and only interpolates
locally in between, so a spent or newly granted stack does not restart the bar on the client. (An
older server that omits the two fields leaves the client running its own clock.) The reply goes out
on the first link message and again every few seconds (which also recovers a client UI reload), so
changing `ManaSpeedFlying.DiveSpeed` on the server is enough -- no matching edit in
`wxl-mana-speedflying.ini` is needed. If the module is not installed, the client falls back to its own
`GlideSpeedPct` / `MaxSpeedPct` defaults.

## The talent protocol -- `WXLTT`

The tree rides a **second prefix**, so the `WXLMS` link parser is untouched and an older client keeps
working against this server. Append only; never reorder or renumber a field.

```
client -> server:  WXLTT<TAB>W<TAB><talentId>   learn one rank
                   WXLTT<TAB>R<TAB><b1|b2|b3>      refund one branch
                   WXLTT<TAB>R<TAB>A              refund the whole tree
                   WXLTT<TAB>Q                  request the current tree
	server -> client:  WXLTT<TAB>T<TAB><points>,<spent>,<unspent>[,<id>:<rank>,...][,P<permille>,N<yards>]
                   WXLTT<TAB>P<TAB><permille>,<yards>   the partial point alone, for the progress bar
                   WXLTT<TAB>F<TAB><reason>     a refusal, echoed for the panel
```

The tally's trailing `id:rank` pairs are a list, so a new talent needs no protocol change, and the
`F` reply means a refusal is always answered rather than silently dropped -- that is what stops the
panel drifting out of step with the server. A tally is sent after every `W`, `R` and `Q`.

The optional trailing `,P<permille>,N<yards>` is the partial point as thousandths and the yards the
next point still needs, for the client's progress bar. It is appended **after** the rank pairs on
purpose: a client parses a pair it cannot read as the end of the list, so an older panel keeps every
rank and only loses the bar.

The bar moves on its own, so it gets its own body rather than a whole tally: `P` carries those two
fields and nothing else, roughly every 500 ms while flight is banking yards and the permille changes.
Answering that with a full tally would drag up to 17 `id:rank` pairs -- several hundred bytes -- along
with two numbers, which is the bulk of what this module used to send. A tally now only goes out when a
rank or a point count can have moved.

Every write validates in this order, inside one lock: config enabled, the player is on a mount, the
character has a progress row, the talent id is known, `rank + 1 <= maxRank`, the tier's pick is free
(skipped for a bridge), the branch is open, every tier above it in the branch is at full rank (skipped
for a bridge), the bridge's *own* branch is open (a bridge is a gate, not a bypass), the lifetime cap,
and only then the derived unspent count. A forged learn message
therefore costs exactly what a real one would, and a forged bridge costs a point like any other rank.

A tier opens top down inside its branch: tiers 2 and 3 need the tier above them full in *every* talent
it holds, so filling a branch costs `3+3+3+1 = 10` points. Tier 2 is a **pick of two** -- the server
refuses the rival of a ranked talent with `tier-taken`, so switching halves needs a respec.

D5 **Aftershock** is the one client→server message that is not the tree itself: it reuses the
`WXLMS` link's state byte with a new letter `X`, sent when the boost key comes up while a boost is
still live. The server checks the rank itself, so an `X` from a character without the talent does
nothing, and matches on count exactly like a real one. An older server that does not know `X` falls
back to its `default:` branch and treats it as a plain level state -- degraded, not broken.

Only client-side talents need a wire field. 14 (Smoothed Plumage, now a tier-1 talent) is feel-only,
so its ranks ride the tally and the client scales its own dive shake and mote density; every other
talent is applied entirely server-side and needs nothing new, which is what keeps the protocol
append-only instead of adding a field per talent.

## Requirements

- Client: `wxl-mana-speedflying` with `ServerLink=1` in `wxl-mana-speedflying.ini` (default on).
- Server: `CONFIG_ADDON_CHANNEL` enabled (AzerothCore default) so the addon whisper is accepted.

## Install

Copy the `mod-mana-speedflying` folder into your AzerothCore `modules/` directory and rebuild the
worldserver. The build copies `mod-mana-speedflying.conf.dist` into `configs/`; rename it to
`mod-mana-speedflying.conf` and edit the values there. The module is picked up automatically (no
`CMakeLists.txt` is needed on current AzerothCore).

## The talent tree

Each character earns **points** by flying on an AdvFly mount, and spends them on 17 talents in three
branches of four tiers. The server owns the tree: the client only renders it. A dive counts
`Talents.DiveAccrual` (1.5) times its distance, earning is capped per rolling minute by
`Talents.MaxPointsPerMinute` (`0` removes the cap; a point leaves the window 60 s after it was paid, so
a steady flight keeps earning at the configured rate), and a tick covering more than
`Talents.TeleportYards` (200) is treated as a teleport and earns nothing. **Dive is the entry branch and
is always open**; the Glide and Wings branches open only when
the 1-point bridge beside them is ranked. **13 points is the lifetime cap** (`Talents.MaxPoints`): a
full branch costs 10 (3+3+3+1), each bridge costs 1, so 13 is a whole branch, both bridges and the
first point they unlock in the branch next door.

The yard cost of each point rises: **`Talents.BaseYards` (500) for the first, then +`Talents.YardStep`
(100%, the default) for every point after it** -- the step *doubles*: 500, 1000, 2000, 4000 ... yd,
about 8.2M yards for all 13. Slipstream
scales the *step* of the next point rather than the rate yards are banked at, so a rank in it can only
ever shorten the tree. The panel's bar shows the 13-step progress and the yards still owed.

| id | branch | tier | name | ranks | effect (percentage of the key above) |
|----|--------|------|------|-------|---------------------------------------|
| 1 | Dive | 1 | Steep Descent | 3 | `DiveMs` -8% per rank |
| 3 | Dive | 2 | Terminal Velocity | 3 | `DiveSpeed` +6% per rank, capped by `SpeedCapPct` |
| 2 | Dive | 2 | Held Breath | 3 | `ClimbDecelMs` +15% per rank |
| 4 | Dive | 3 | Shallow Pull | 3 | a pull-out 15 degrees shallower per rank keeps its whole glide |
| 5 | Dive | 4 | Aftershock | 1 | releasing the boost key into a dive commits the burst at once |
| 6 | Glide | 1 | Long Glide | 3 | `GlideMs` +12% per rank |
| 7 | Glide | 2 | Slipstream | 3 | the next point's `YardStep` -12% per rank |
| 8 | Glide | 2 | Banked Momentum | 3 | boost carry +10 points of `BoostCarryPct` per rank |
| 10 | Glide | 3 | Skyward Path | 3 | `LevelSpeed` +10% per rank of glide elevation |
| 9 | Glide | 4 | Twin Vault | 1 | `BoostMaxStacks` +1: a **fourth** boost stack |
| 14 | Wings | 1 | Smoothed Plumage | 3 | client-side only: dive shake, speed-mote density |
| 12 | Wings | 2 | Steady Wings | 3 | `StackIntervalMs` -8% per rank |
| 11 | Wings | 2 | Diving Wings | 3 | `StackDiveIntervalMs` -10% per rank |
| 13 | Wings | 3 | Iron Wings | 3 | `BoostAdd` +8% per rank |
| 15 | Wings | 4 | Rising Current | 1 | `ClimbDecelMs` +40%: a climb keeps its entered speed |
| 16 | Dive | 3 | Still Air | 1 | climb threshold 15° -> 25°, **and opens the Glide branch** |
| 17 | Glide | 3 | Tucked Wings | 1 | dive threshold 35° -> 45°, **and opens the Wings branch** |

- **Branch gate (`tree-locked`).** Dive is the entry branch and is always open. The Glide branch opens
  on a rank in 16 Still Air; the Wings branch opens on a rank in 17 Tucked Wings. A talent in a closed
  branch is refused, so the gate cannot be walked around by buying the branch directly. A bridge is
  refused the same way when the branch it *sits in* is closed: 17 lives in Glide, so Wings cannot be
  opened straight from Dive -- 16 must be bought first. The graph is a chain, Dive -> 16 -> 17.
- **Tier gate.** Tier 1 is free to buy. Tiers 2 and 3 open only once the tier above them is at full
  rank (in every talent it holds), so a branch is 3+3+3+1 = 10 points. Tier 4 is a single-rank capstone.
  Bridges (16, 17) are skipped by that rule: they sit beside their branch's own tier-3 talent and would
  otherwise make the tier need 4 points and put the capstone out of reach. A bridge also needs no tier
  above it -- it is the way in.
- **Tier 2 is a pick.** Of the two talents in a tier-2 group only one may hold a rank; the server
  refuses the rival with `tier-taken`. Switch halves by resetting the branch (`Reset`, or right-click a
  talent). A bridge is exempt -- it never competes with its branch's own pick, so Shallow Pull and Still
  Air can be ranked together.
- **A refund re-opens nothing.** Refunding a bridge refunds its rank only: points already spent in the
  branch it opened keep their rank and merely stop accepting new ones until the bridge is bought again.
  That is what keeps a respec from destroying a tree in one click.
- **Both bridges are client-side.** They carry no `E_*` effect: the angle they widen is a client-side
  threshold, so the ranks ride the tally like 14 Smoothed Plumage and the effect is applied by
  `wxl-mana-speedflying`. The server-side consequence is exactly one row each -- the rank, not a speed.
- **No effect is ever applied twice.** D2 and W15 both move `ClimbDecelMs`; their ranks are summed
  into one multiplier, not compounded. D3 is the only talent that raises a state speed, and the boost
  ceiling is derived from the fastest state -- so `SpeedCapPct` is applied to the base states *before*
  that derivation, or the clamp would be silently undone.
- **Per-player, not global.** The effects live in a per-character copy of the config (`Effective cfg`),
  rebuilt on login, on every rank bought and on every config reload; the base keys are never mutated.
  Idle players are not walking around with the server's global speeds.
- **Persistence.** `character_speedflying` holds lifetime points and the sub-point yard fraction;
  `character_speedflying_talents` holds one row per unrefunded rank. Unspent points are derived
  (`points - SUM(rank)`) and never stored, so the two cannot drift. Dirty rows are flushed on a
  `Talents.FlushMs` (20 s) timer, on logout and on world shutdown. Respec (`WXLTT\tR\t<b1|b2|b3|A>`) is
  free by default and
  takes the `Talents.RespecCost` if set; it refuses rather than half-applies when the money is missing.
  Deleting a character deletes both of its rows, so a recycled guid cannot inherit a tree.
Points are keyed on the character's low guid (`characters.guid`, i.e. `GetGUID().GetCounter()`), and the
module cannot add a prepared statement -- the `CharacterDatabaseStatements` enum lives in the core -- so
the queries interpolate integers only, never a string.

## Configuration

All keys live under `[worldserver]`; see `conf/mod-mana-speedflying.conf.dist` for the full
comments. Defaults:

```ini
ManaSpeedFlying.Enable       = 1
ManaSpeedFlying.ClimbSpeed   = 2.0    ; 200%
ManaSpeedFlying.LevelSpeed   = 2.8    ; 280%
ManaSpeedFlying.DiveSpeed    = 8.0    ; 800%
ManaSpeedFlying.BlendMs      = 5500   ; default blend, proportional to the span it covers
ManaSpeedFlying.DiveMs       = 4000   ; accelerate into a dive
ManaSpeedFlying.GlideMs      = 18000  ; long glide from a dive back to level
ManaSpeedFlying.ClimbDecelMs = 3500   ; dive -> climb: hold the speed, then brake hard
ManaSpeedFlying.HoverMs      = 500    ; fixed accel/decel zone leaving a hover
ManaSpeedFlying.SendMs       = 100    ; blend step; each change is broadcast, so the rate is quantized to 0.01
ManaSpeedFlying.TimeoutMs    = 2500   ; restore the mount's own speed if the link goes quiet
ManaSpeedFlying.BoostAdd     = 4.0    ; 400%: speed one boost adds to the glide
ManaSpeedFlying.BoostSpeed   = 16.0   ; 1600%: hard boost ceiling (raised to the fastest state
                                     ; speed if set lower)
ManaSpeedFlying.BoostFadeMs  = 4000   ; how fast the burst bleeds back to the state speed
ManaSpeedFlying.BoostCarryPct = 0.50  ; fraction of the boost folded into the glide
ManaSpeedFlying.BoostGlidePct = 0.70  ; fraction of GlideMs a boost's carry may spend
ManaSpeedFlying.BoostMaxStacks  = 3   ; 0..3, recharged on a timer
ManaSpeedFlying.StackIntervalMs     = 9000 ; normal-flight stack recharge
ManaSpeedFlying.StackDiveIntervalMs = 3000 ; diving stack recharge
ManaSpeedFlying.SpeedCapPct  = 1600.0 ; ceiling a talent may raise a base state speed to

; Talent tree (see "The talent tree" below; every effect is a percentage of the key above it)
Talents.Enable             = 1   ; master switch for earning and spending
Talents.MaxPoints          = 13  ; lifetime cap on earned points
Talents.BaseYards          = 500.0 ; yards for the first point
Talents.YardStep           = 1.0   ; each point after the first costs this much more (1.0 = doubles)
Talents.MaxPointsPerMinute = 4   ; rolling-window cap; 0 = uncapped
Talents.RespecCost         = 0   ; copper per respec; 0 = free
Talents.FlushMs            = 20000 ; how often dirty points/ranks are written back
Talents.TeleportYards      = 200.0 ; a tick longer than this is a teleport, not flight
Talents.DiveAccrual        = 1.5 ; distance multiplier while diving
```

- **BlendMs** is the time for the *whole* climb-to-dive span. The rate is linear, so a smaller
  hop (for example level to climb) takes proportionally less. Raise it for a longer blend.
  **GlideMs** and **ClimbDecelMs** are likewise scaled by the speed actually being shed, so a
  shallow pull-out recovers faster than a full dive's.
- The speeds are multipliers of base run speed, the same scale `SetSpeed(MOVE_FLIGHT, ...)` uses: a
  stock 280% mount is `2.8`.
- Only flying players are touched, and only the forward flight speed; ground speed, taxis and
  vehicles are left alone.

### The button boost

Boost stacks recharge on a timer: one every `StackIntervalMs` (9 s) in normal flight, or every
`StackDiveIntervalMs` (3 s) while diving, up to `BoostMaxStacks`. The timer only runs while the mount
is link-eligible (flying, AdvFly mount), so a grounded mount earns nothing. Each press of the client's
boost button (can be used while climbing, diving or gliding level) spends one stack and adds `BoostAdd` (400% by
default) to the mount's current speed, up to the `BoostSpeed` ceiling. There is no cooldown, so held
stacks can be chained into one bigger burst. `BoostFadeMs` is how long the burst takes to bleed back
to the glide the mount already had, and `BoostCarryPct` folds a slice of every boost into the
ordinary glide (it decays on the glide curve), so a boost is still felt seconds after the
burst is gone. `BoostGlidePct` caps how much glide that slice may buy: a full `BoostAdd` of carry
buys `BoostGlidePct` of the ordinary `GlideMs` (0.7 by default, so a boost sheds before the 9 s
stack recharge instead of keeping the mount boosted all the time), a boost that lands on a
running glide extends that glide by only its own carry rather than restarting the long pull-out
curve, and chained or banked stacks cannot buy more than one boost's worth. The carried speed also
never lifts the base glide above `DiveSpeed`: anything past it is speed no plain state reaches, so
it stays in the short burst and fades on `BoostFadeMs` -- a boost spent at the dive ceiling empties
on the fade instead of stretching the pull-out that follows. A boost spent while climbing has
nowhere to fold *during* the climb -- a climb is not
a glide -- so its carried half is banked, the other half is the short burst, and the speed settles
back to `ClimbSpeed` on the climb's own transition. The moment you level out, the banked half is
released onto an ease-in glide sized by the carry: a full `BoostAdd` of carry buys `BoostGlidePct`
of a `GlideMs`, a smaller carry proportionally less. So the speed you built climbing does not empty
in a few seconds.

## Notes

- The client→server channel is an addon whisper to self. It is throttled on the client (about one
  message per 200 ms, plus a 1 s heartbeat) and the server treats it as untrusted input: a `1` link
  is only honoured for a flying player, a boost off the ground is answered but not applied, and a
  message flood is dropped. The server only ever sets a flight speed, so a forged link can at worst
  ask for one of the three configured state speeds -- or, while actually flying, spend the same boost
  stacks a legitimate client would have had to build.
- If the client module is unloaded, disabled or crashes mid-flight, `TimeoutMs` restores the mount's
  own speed and the module stands down until the next link message.
- Some servers apply mount-speed auras that recompute the flight rate when mounting or on buffs. The
  module re-applies its blended value on the next update, so the two may briefly fight; disable one
  or the other if that shows.

## Build

Standard AzerothCore module: drop into `modules/` and build the worldserver. No extra dependencies.
