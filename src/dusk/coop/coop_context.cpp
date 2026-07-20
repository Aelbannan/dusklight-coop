#include "dusk/coop/coop_context.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_debug.h"

#include <vector>

namespace dusk::coop {
namespace {

thread_local std::vector<ContextFrame> g_stack;
thread_local ContextFrame g_default{};

}  // namespace

ScopedContext::ScopedContext(ContextFrame frame) {
    if (!isEnabled()) {
        return;
    }
    previous_ = currentContext();
    g_stack.push_back(frame);
    auto& rt = runtime();
    rt.activePlayer = frame.player;
    rt.activeView = frame.view;
    rt.activeEnemyTarget = frame.enemyTarget;
    pushed_ = true;
}

ScopedContext::~ScopedContext() {
    if (!pushed_) {
        return;
    }
    if (!g_stack.empty()) {
        g_stack.pop_back();
    }
    auto& rt = runtime();
    if (g_stack.empty()) {
        rt.activePlayer = previous_.player;
        rt.activeView = previous_.view;
        rt.activeEnemyTarget = previous_.enemyTarget;
    } else {
        const auto& top = g_stack.back();
        rt.activePlayer = top.player;
        rt.activeView = top.view;
        rt.activeEnemyTarget = top.enemyTarget;
    }
}

const ContextFrame& currentContext() {
    if (g_stack.empty()) {
        g_default.player = runtime().activePlayer;
        g_default.view = runtime().activeView;
        g_default.enemyTarget = runtime().activeEnemyTarget;
        return g_default;
    }
    return g_stack.back();
}

PlayerId currentPlayer() { return currentContext().player; }
ViewId currentView() { return currentContext().view; }
fopAc_ac_c* currentEnemyTarget() { return currentContext().enemyTarget; }

void assertContextStackEmpty() {
    COOP_ASSERT(g_stack.empty());
}

}  // namespace dusk::coop
