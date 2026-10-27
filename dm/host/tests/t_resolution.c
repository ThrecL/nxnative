// Exercise production memory policy with native/old/wrong-build images, at
// every FPS choice. No game assets or executable guest code are needed.
#include "../hle_fpslock.c"
module_t g_mod[MAX_MOD];int g_nmod=1;
static int writes;
void dm_guest_write_prepare(void *p,size_t n){(void)p;(void)n;writes++;}
static int checks,failed;
#define CHECK(c) do{checks++;if(!(c)){failed++;fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);}}while(0)
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    if(!strcmp(argv[1],"unset"))unsetenv("DM_DYNAMIC_RESOLUTION");else setenv("DM_DYNAMIC_RESOLUTION",argv[1],1);
    setenv("DM_FPS_TARGET",argv[2],1);
    uint8_t *base=calloc(1,0x1a00000),*device=calloc(1,0xa0000);
    strcpy(g_mod[0].name,"main");strcpy(g_mod[0].build_id,"3c10ec35c5ef4c3686b4053afb0e58e7");g_mod[0].base=base;
    int wrong=!strcmp(argv[3],"wrong"),old=!strcmp(argv[3],"old");
    if(wrong)memset(g_mod[0].build_id,'a',32);
    *(uint32_t *)(base+0x9bf568)=old?0x1e2e1000:0x1e254c80;
    *(uint32_t *)(base+0x9bf578)=old?0xd503201f:0x54000489;
    *(uint64_t *)(base+0x9bf528)=0x72a7f00c5280000cull;
    *(uint64_t *)(base+0x9bf534)=0x72a7f00c5280000cull;
    *(uint64_t *)(base+0x192be00)=(uint64_t)device;
    *(uint32_t *)(device+0x9c160)=1;
    uint32_t *scale=(uint32_t *)(base+0x128e068);scale[0]=0x3f800000;scale[1]=0x3f666666;
    *(uint32_t *)(base+0x194f698)=7;
    dm_fps_lock_tick();
    int dynamic=!strcmp(argv[1],"1")&&!old&&!wrong,target=atoi(argv[2]);
    CHECK(scale[0]==(dynamic?0x3f333333u:0x3f800000u));
    CHECK(scale[1]==(dynamic||wrong?0x3f666666u:0x3f800000u));
    CHECK(*(uint32_t *)(base+0x194f698)==7); // never suppress the native counter
    CHECK(*(uint32_t *)(device+0x9c160)==(!wrong&&target?60/target:1));
    CHECK(*(uint32_t *)(base+0x192c304)==(!wrong&&target==30?1:0));
    if(dynamic){scale[1]=0x3f59999a;dm_fps_lock_tick();CHECK(scale[1]==0x3f59999a);}
    if(wrong)CHECK(writes==0);
    printf("t_resolution %s fps%s %s: %d checks, %d failed\n",argv[1],argv[2],argv[3],checks,failed);
    free(device);free(base);return failed!=0;
}
