#include "engine_constructor_run.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool gate_open;
    int fail_begin;
    int fail_completed;
    uintptr_t called[8];
    size_t calls;
    size_t began;
    size_t finished;
} TestContext;

static bool gate(void *opaque)
{
    return ((TestContext *)opaque)->gate_open;
}

static bool begin(size_t index, uintptr_t target, void *opaque)
{
    TestContext *ctx = opaque;
    assert(index == ctx->began);
    assert(target == (uintptr_t)(0x100u + index * 4u));
    ++ctx->began;
    return (int)index != ctx->fail_begin;
}

static void invoke(uintptr_t target, void *opaque)
{
    TestContext *ctx = opaque;
    assert(ctx->began == ctx->calls + 1);
    assert(target == (uintptr_t)(0x100u + ctx->calls * 4u));
    ctx->called[ctx->calls++] = target;
}

static bool completed(size_t index, uintptr_t target, void *opaque)
{
    TestContext *ctx = opaque;
    assert(index == ctx->finished);
    assert(target == ctx->called[index]);
    ++ctx->finished;
    return (int)index != ctx->fail_completed;
}

static TestContext fresh(void)
{
    TestContext ctx = {0};
    ctx.gate_open = true;
    ctx.fail_begin = -1;
    ctx.fail_completed = -1;
    return ctx;
}

int main(void)
{
    uintptr_t targets[] = {0x100, 0x104, 0x108};
    EngineConstructorPlan plan = {
        .status = ENGINE_CONSTRUCTORS_VALIDATED,
        .targets = targets,
        .call_count = 3,
    };
    EngineConstructorRunState state = {0};
    TestContext ctx = fresh();

    ctx.gate_open = false;
    assert(!engine_constructor_run_once(&plan, &state, gate, begin, completed,
                                        invoke, &ctx));
    assert(!state.started && !ctx.calls && !ctx.began);
    ctx.gate_open = true;
    assert(engine_constructor_run_once(&plan, &state, gate, begin, completed,
                                       invoke, &ctx));
    assert(state.started && state.completed && state.next_index == 3);
    assert(ctx.calls == 3 && ctx.began == 3 && ctx.finished == 3);
    assert(!engine_constructor_run_once(&plan, &state, gate, begin, completed,
                                        invoke, &ctx));
    assert(ctx.calls == 3);

    state = (EngineConstructorRunState){0};
    ctx = fresh();
    ctx.fail_begin = 1;
    assert(!engine_constructor_run_once(&plan, &state, gate, begin, completed,
                                        invoke, &ctx));
    assert(state.started && !state.completed && state.next_index == 1);
    assert(ctx.calls == 1 && ctx.began == 2 && ctx.finished == 1);
    assert(!engine_constructor_run_once(&plan, &state, gate, begin, completed,
                                        invoke, &ctx));

    state = (EngineConstructorRunState){0};
    ctx = fresh();
    ctx.fail_completed = 0;
    assert(!engine_constructor_run_once(&plan, &state, gate, begin, completed,
                                        invoke, &ctx));
    assert(state.started && !state.completed && state.next_index == 0);
    assert(ctx.calls == 1 && ctx.began == 1 && ctx.finished == 1);

    state = (EngineConstructorRunState){0};
    ctx = fresh();
    plan.status = ENGINE_CONSTRUCTORS_INVALID;
    assert(!engine_constructor_run_once(&plan, &state, gate, begin, completed,
                                        invoke, &ctx));
    assert(!state.started && ctx.calls == 0);

    plan.status = ENGINE_CONSTRUCTORS_NO_ENTRIES;
    plan.call_count = 0;
    plan.targets = NULL;
    assert(engine_constructor_run_once(&plan, &state, gate, begin, completed,
                                       invoke, &ctx));
    assert(state.started && state.completed && state.next_index == 0);
    assert(ctx.calls == 0);

    puts("engine_constructor_run_test: gating, ordering and once-only journals passed");
    return 0;
}
