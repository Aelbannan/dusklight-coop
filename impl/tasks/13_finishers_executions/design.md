# Finishers and Executions — Design

## Move classes to audit

Ending Blow finish cuts, finishing stab, down cut, Mortal Draw phases, wolf jump finish, heavy jump attacks, Helm Splitter, Back Slice, bomb/iron-ball special weaknesses, enemy-specific elemental instant kills.

## Attack resolution

```cpp
enum class AttackResolution {
    NormalDamage,
    HeavyDamage,
    Execution,
    NativeSpecial,
    ScriptedKill,
};
```

Do not classify as execution based only on high damage.

## Execution claim

Only one player may claim the execution when target is in legitimate finishable state.

```cpp
struct ExecutionClaim {
    PlayerId player;
    uint32_t expiresAtFrame;
};
```

During execution: other players cannot claim it; other attacks should not knock target away; execution camera affects claiming player's viewport; global simulation may use short hit-stop if required.

## Mortal Draw

Do not globally set enemy health to zero. Preserve true enemy-specific execution behavior. If implemented as high damage, run through durability scaling. Never globally execute bosses. Preserve enemy-specific armor-break or hit-count behavior.

## Wolf finishers

Same execution-claim model as Ending Blow. Normal wolf jump remains normal/heavy damage. Only explicit finish variants bypass ordinary durability.

## Player-side instant deaths

| Source | Co-op handling |
|--------|---------------|
| Normal lethal enemy hit | Down the player |
| Enemy grab execution | Down captured player |
| Crush | Down player |
| Lava | Down player and reposition |
| Void | Respawn near living teammate with penalty |
| Out-of-bounds | Teleport safely |
| All players downed | Normal game over |
| Story-scripted capture | Preserve original global event |
