# Proof-of-Concept Gates

Gates are mandatory checkpoints. Implementation does not advance past a gate until the listed evidence exists. Gates must be cleared in order where they have dependencies.

## Gate status

| Gate | Description | Status | Depends on |
|------|-------------|--------|------------|
| **A** | Render replay boundary | 🟡 PoC implemented | — |
| **B** | Secondary camera safety | 🟡 PoC implemented | — |
| **C** | Input abstraction | 🟡 PoC implemented | — |
| **D** | Proxy player → Phase 6 Link | 🟡 Phase 6 secondary Alink | A, B, C |
| **E** | Resource adapter | 🟡 PoC implemented | — |
| **F** | Combat attribution | 🟡 PoC implemented | D, E |
| **G** | One enemy adapter | 🟡 PoC implemented | D, F |
| **H** | Difficulty and drops | 🟡 PoC implemented | E, G |
| **I** | Independent forms | 🟡 PoC implemented | D |
| **J** | Multiple Eponas | 🟡 PoC implemented | D |

**Legend:** ❌ Not started 🟡 In progress ✅ Complete

## Gate completion checklist template

Each gate file contains:
- **Deliverables:** What must be produced
- **Acceptance criteria:** How success is measured
- **Evidence log:** Track completed evidence items
- **Sign-off:** Gate keeper and date
