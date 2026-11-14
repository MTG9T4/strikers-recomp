/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strikers project code. Resume lookup contract follows
 * ssbu-recomp/runtime/nnunwind.c (de221d22f867406b4204dbd580404f603ee19b1a).
 * Generic lifted-image registration replaces the Smash-specific NRO lookup. */
#include <windows.h>
#include "eh_runtime.h"
typedef struct { uint64_t base,size; const EhResumeEntry *table; int count; } ResumeImage;
static ResumeImage g_images[16];
static SRWLOCK g_lock = SRWLOCK_INIT;
int eh_register_resume_table(uint64_t base,uint64_t size,const EhResumeEntry *table,int count) {
    if (!size || base+size < base || count<0 || (count && !table)) return 0;
    for(int i=0;i<count;i++)
        if (!table[i].fn || table[i].lp<base || table[i].lp-base>=size ||
            (i && table[i-1].lp>=table[i].lp)) return 0;
    int slot=-1;
    AcquireSRWLockExclusive(&g_lock);
    for(int i=0;i<16;i++) {
        if(g_images[i].base==base && g_images[i].size) {slot=i;break;}
        if(!g_images[i].size && slot<0)slot=i;
    }
    if(slot>=0) { ResumeImage im={base,size,table,count};g_images[slot]=im; }
    ReleaseSRWLockExclusive(&g_lock);
    return slot>=0;
}
void eh_unregister_resume_table(uint64_t base) {
    AcquireSRWLockExclusive(&g_lock);
    for(int i=0;i<16;i++) if(g_images[i].base==base) {ResumeImage empty={0};g_images[i]=empty;}
    ReleaseSRWLockExclusive(&g_lock);
}
EhResumeFn eh_resume_entry(uint64_t lp) {
    EhResumeFn fn=eh_main_resume_entry(lp);
    if(fn)return fn;
    AcquireSRWLockShared(&g_lock);
    for(int i=0;i<16;i++) {
        const ResumeImage *im=&g_images[i];
        if(!im->size || lp<im->base || lp-im->base>=im->size)continue;
        int lo=0,hi=im->count-1;
        while(lo<=hi) {int mid=lo+(hi-lo)/2;if(im->table[mid].lp==lp) {fn=im->table[mid].fn;break;}
            if(im->table[mid].lp<lp)lo=mid+1;else hi=mid-1;}
        break;
    }
    ReleaseSRWLockShared(&g_lock);
    return fn;
}
