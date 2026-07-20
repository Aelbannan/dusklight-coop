# Goals and Final Design Decisions

## Core goal

Add local split-screen co-op to Dusklight while preserving the original single-player game and save format as much as possible.

## Requirements

- Two players first, with a path toward more players.
- More than four physical controllers on PC.
- Multiple Link actors in one shared game world.
- Multiple independently controlled cameras and viewports.
- One shared room and one shared simulation.
- Shared permanent items and progression.
- Individual consumable resources, bottle contents, rupees.
- Individual player health, damage, targeting, and combat state.
- More ordinary enemies for larger parties.
- A separate global difficulty system.
- Free-for-all pickup collection.
- No centralized enemy attack coordination.
- Optional soft attention distribution.

## Final gameplay ownership model

| State | Ownership |
|---|---|
| Permanent item unlocks | Global |
| Sword, shield, armor availability | Global |
| Bow, boomerang, clawshot, spinner, Dominion Rod, Iron Boots, etc. | Global |
| Quest flags and story progression | Global |
| Dungeon keys, boss keys, map, compass | Global |
| Opened chests and room completion switches | Global |
| Number of unlocked bottle slots | Global |
| Equipped action-button items | Per player |
| Equipped sword, shield, and armor choice | Per player |
| Health and current magic | Per player |
| Arrows and slingshot ammo | Per player |
| Bomb bag contents and bomb counts | Per player |
| Lantern oil | Per player |
| Bottle contents and bottle quantities | Per player |
| Rupees | Per player |
| Lock-on target | Per player |
| Invulnerability and hit reaction | Per player |
| Downed/revival state | Per player |
| Human or wolf form | Per player |
| Wolf senses state | Per player and per viewport |
| Wolf Midna rider visuals and field state | Per player |
| Epona actor and mounted state | One per player |
| Horse unlock/story availability | Global |
| Enemy health and AI state | Shared |
| Boss phase and health | Shared |

## Final encounter rules

- Ordinary enemies may be duplicated using a whitelist.
- Elites, bosses, puzzle enemies, and scripted enemies are not duplicated automatically.
- Enemy attacks are not coordinated through attack tokens.
- Each enemy independently selects and attacks its target.
- A lightweight distribution penalty may encourage enemies to spread across players.
- Enemy attack animations remain at vanilla speed by default.
- Global difficulty may reduce enemy decision and cooldown delays.
- Extra enemy count is the primary co-op scaler.
- Enemy health and stagger resistance increase modestly.
- Enemy damage is primarily controlled by the selected global difficulty, not player count.
- Drops are budgeted at the encounter level and collected free-for-all.

## Initial scope limitation

> All players occupy the same loaded room and stage context.

No independent room streaming, rollback, network sync, or boss duplication in the first release.

## Mandatory eight-viewport decision

> Eight concurrently active, independently controlled cameras rendered into eight simultaneous viewports.

Shared cameras may exist as optional layouts but are not an acceptable fallback for Players 5-8.

## Mandatory independent-form and multi-Epona decision

Every player may independently be human or wolf, and every player may own, call, mount, and ride a separate Epona actor. This includes:

- per-player transformation state and animation;
- per-player wolf collision, actions, senses, scent, digging, howling, and combat;
- per-player Wolf Midna rider visuals and Midna-field targeting;
- per-player horse ownership, summoning, mounting, riding, horseback combat, audio, and camera;
- persistence and transitions for eight player forms and eight horse slots.
