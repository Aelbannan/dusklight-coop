#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop {

class ScopedContext {
public:
    explicit ScopedContext(ContextFrame frame);
    ~ScopedContext();

    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    ContextFrame previous_{};
    bool pushed_ = false;
};

const ContextFrame& currentContext();
bool hasScopedContext();
PlayerId currentPlayer();
ViewId currentView();
fopAc_ac_c* currentEnemyTarget();

void assertContextStackEmpty();

}  // namespace dusk::coop
