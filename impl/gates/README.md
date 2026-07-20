# Proof-of-Concept Gates

Gates are mandatory checkpoints. Implementation does not advance past a gate until the listed evidence exists. Gates must be cleared in order where they have dependencies.

## Gate status

| Gate | Description | Status | Depends on |
|------|-------------|--------|------------|
| **A** | Render replay boundary | ❌ Not started | — |
| **B** | Secondary camera safety | ❌ Not started | — |
| **C** | Input abstraction | ❌ Not started | — |
| **D** | Proxy player | ❌ Not started | A, B, C |
| **E** | Resource adapter | ❌ Not started | — |
| **F** | Combat attribution | ❌ Not started | D, E |
| **G** | One enemy adapter | ❌ Not started | D, F |
| **H** | Difficulty and drops | ❌ Not started | E, G |
| **I** | Independent forms | ❌ Not started | D |
| **J** | Multiple Eponas | ❌ Not started | D |

**Legend:** ❌ Not started 🟡 In progress ✅ Complete

## Gate completion checklist template

Each gate file contains:
- **Deliverables:** What must be produced
- **Acceptance criteria:** How success is measured
- **Evidence log:** Track completed evidence items
- **Sign-off:** Gate keeper and date
