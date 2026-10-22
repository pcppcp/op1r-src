/* Local harness for the unmodified SDK loader and descriptor validator. */
#include "kennel/kn_load.h"
#include "kennel/kn_validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
int main(int argc,char **argv)
{
    assert(argc>=4);
    FILE *f=fopen(argv[1],"rb");assert(f);
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    uint8_t *file=malloc((size_t)size);assert(file);
    assert(fread(file,1,(size_t)size,f)==(size_t)size);fclose(f);
    kn_info info;const char *error=0;
    if(kn_parse(file,(uint32_t)size,1,&info,&error)){fprintf(stderr,"%s\n",error);return 1;}
    kn_export exports[16];assert(argc-4<=16);
    for(int i=4;i<argc;i++){
        exports[i-4].name=argv[i];exports[i-4].addr=0x01e00100+(i-4)*0x40;
    }
    uint32_t base=(uint32_t)strtoul(argv[2],0,0);
    uint8_t *out=malloc(info.load_size);assert(out);
    if(kn_relocate(file,&info,out,base,info.load_size,exports,argc-4,&error)||
       kn_validate(out,base,info.image_size,info.text_size,info.kind,0,info.entry_offset,info.screen_offset,&error)){
        fprintf(stderr,"%s\n",error);return 1;
    }
    f=fopen(argv[3],"wb");assert(f);
    assert(fwrite(out,1,info.load_size,f)==info.load_size);fclose(f);
    free(file);free(out);return 0;
}
