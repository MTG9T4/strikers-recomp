/* Developer-only native acceptance driver. Never part of the player host.
 * Uses the same group APIs as F1 and ordinary replay input; no guest writes. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "group_client.h"
#include "zt_transport.h"
#include "pc_controls.h"
extern long long display_frames(void);
extern int display_closing(void);
static HANDLE driver_thread;
static volatile LONG driver_stopping;

static void capture(const char *label) {
    printf("[group-selftest] capture=%s frame=%lld\n",label,display_frames());fflush(stdout);
    pc_capture_request();
}
static DWORD WINAPI drive(void *unused) {
    char role[32]={0},name[129]={0};(void)unused;
    GetEnvironmentVariableA("STRIKERS_GROUP_TEST_ROLE",role,sizeof role);
    GetEnvironmentVariableA("STRIKERS_GROUP_TEST_NAME",name,sizeof name);
    if(!role[0])return 0;
    while(!driver_stopping&&!display_frames())Sleep(50);
    if(driver_stopping)return 0;
    Sleep(500); /* The ordinary host completed CRT/options and its first present. */
    int member=strcmp(role,"outsider")!=0,joined=0,friends_shot=0,host_shot=0,invited=0,received=0,outsider_shot=0;
    if(member)group_start();
    ULONGLONG end=GetTickCount64()+2400000,last=0;
    while(!driver_stopping&&!display_closing()&&GetTickCount64()<end) {
        GroupStatus g;group_status(&g);ZtStatus z;zt_status(&z);long long frame=display_frames();
        if(member&&!joined&&g.configured&&z.node&&!g.busy){
            char code[16];memcpy(code,g.code,sizeof code);code[15]=0;
            group_join(code,name);SecureZeroMemory(code,sizeof code);joined=1;
            printf("[group-selftest] join queued role=%s node=%010llx\n",role,(unsigned long long)z.node);fflush(stdout);
        }
        SecureZeroMemory(g.code,sizeof g.code);
        if(member&&g.joined&&g.count>=2&&z.state==2&&!friends_shot&&frame>=2400) {
            if(!pc_settings_open())pc_settings_toggle();Sleep(300);
            capture(!strcmp(role,"host")?"host-friends":"station-friends");Sleep(1200);
            if(pc_settings_open())pc_settings_toggle();friends_shot=1;
        }
        int published=0;const char *peer=NULL;
        for(unsigned i=0;i<g.count;i++){
            if(!strcmp(g.friends[i].node,g.self)&&!strcmp(g.friends[i].status,"hosting"))published=1;
            if(strcmp(g.friends[i].node,g.self))peer=g.friends[i].node;
        }
        if(!strcmp(role,"host")&&g.hosting&&published&&peer&&!g.busy&&!invited) {
            if(!pc_settings_open())pc_settings_toggle();Sleep(300);capture("host-hosting-badge");Sleep(1200);
            if(pc_settings_open())pc_settings_toggle();host_shot=1;
            group_invite(peer);invited=1;printf("[group-selftest] invite queued to=%s\n",peer);fflush(stdout);
        }
        if(!strcmp(role,"station")&&g.invite_id[0]&&!received) {
            Sleep(300);capture("invite-toast-station");received=1;
            printf("[group-selftest] invite received settings_open=%d\n",pc_settings_open());fflush(stdout);
        }
        if(!member&&frame>=6000&&!outsider_shot){capture("outsider-no-room");outsider_shot=1;}
        if(GetTickCount64()-last>=5000) {
            last=GetTickCount64();char text[256];group_connection(text,sizeof text);
            printf("[group-selftest] role=%s frame=%lld joined=%d reachable=%d friends=%u hosting=%d transport=%d host_shot=%d connection=%s message=%s\n",role,frame,g.joined,g.reachable,g.count,g.hosting,z.state,host_shot,text,g.message);fflush(stdout);
        }
        Sleep(50);
    }
    return 0;
}
static void stop_driver(void){InterlockedExchange(&driver_stopping,1);if(driver_thread){WaitForSingleObject(driver_thread,2000);CloseHandle(driver_thread);}}
static void start_driver(void){driver_thread=CreateThread(NULL,0,drive,NULL,0,NULL);if(driver_thread)atexit(stop_driver);}
#pragma section(".CRT$XCU",read)
__declspec(allocate(".CRT$XCU")) void (*job23b_group_init)(void)=start_driver;
#pragma comment(linker,"/include:job23b_group_init")
