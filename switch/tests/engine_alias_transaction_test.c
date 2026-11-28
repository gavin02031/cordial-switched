#include "engine_alias_transaction.h"
#include <assert.h>
#include <stdio.h>

typedef struct { unsigned map, protect, unmap; unsigned calls[3]; } Fake;
static uint32_t map(void *p) { Fake *f=p; f->calls[0]++; return f->map; }
static uint32_t protect(void *p) { Fake *f=p; f->calls[1]++; return f->protect; }
static uint32_t unmap(void *p) { Fake *f=p; f->calls[2]++; return f->unmap; }
int main(void)
{
    Fake f={.map=11};
    EngineAliasOperations ops={map,protect,unmap,&f};
    EngineAliasResult r=engine_alias_prepare(&ops);
    assert(!r.ready && !r.alias_retained && r.error==11);
    assert(f.calls[0]==1 && !f.calls[1] && !f.calls[2]);
    f=(Fake){.protect=22};
    r=engine_alias_prepare(&ops);
    assert(!r.ready && !r.alias_retained && r.error==22);
    assert(f.calls[0]==1 && f.calls[1]==1 && f.calls[2]==1);
    f=(Fake){.protect=22,.unmap=33};
    r=engine_alias_prepare(&ops);
    assert(!r.ready && r.alias_retained && r.error==33);
    f=(Fake){0};
    r=engine_alias_prepare(&ops);
    assert(r.ready && r.alias_retained && !r.error);
    assert(f.calls[0]==1 && f.calls[1]==1 && !f.calls[2]);
    puts("Alias setup failures retain ownership until rollback succeeds");
}
