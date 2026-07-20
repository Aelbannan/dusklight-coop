# Unanswered-Question Audit

| Question | Resolution |
|---|---|
| Are items separate? | Permanent items global; mutable resources individual |
| Are bottle unlocks individual? | No; slot unlock mask global, contents individual |
| Where is Player 0 data saved? | Original save only |
| Where are secondary resources saved? | Versioned per-slot companion |
| Who controls story transitions? | Player 0 |
| Can players occupy separate rooms? | No, not initially |
| Can there be >4 controllers? | Yes, through Aurora instance IDs |
| Can there be ≥8 independent cameras? | Yes; eight is mandatory |
| What happens with 5-8 players? | Each receives independent viewport (3×2 or 4×2) |
| Are enemy attacks coordinated? | No |
| Is enemy attention distributed? | Optional soft score penalty |
| Is enemy attack animation sped up? | No by default; only cooldown/decision delays |
| Is enemy damage scaled by player count? | No by default |
| Is difficulty separate from party scaling? | Yes |
| Are drops personal? | No, free-for-all |
| Are clone drops unlimited? | No, shared encounter credits |
| Who gets a new bottle's initial contents? | Collector |
| Can a full player consume a pickup? | No; remains for others |
| Is transformation independent? | Yes; every player has independent human/wolf form |
| Who owns Midna? | Each wolf Link owns rider visuals; one canonical standalone for story events |
| Who can ride Epona? | Every player can own, call, mount, and ride their own Epona |
| What audio listener is used? | Party centroid, Camera 0 orientation |
| What happens on one player's death? | Personal fairy, then downed/revive |
| When does game over occur? | All active players downed |
| Are bosses automatically duplicated/scaled? | No |
| Is this an SDK-only mod? | No; core fork changes required |
| Is networking included? | No |
