/* SPDX-License-Identifier: GPL-3.0-or-later; purpose-built bounded JSON parser. */
#include "group_json.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
typedef struct {const char *s;int p,n,cap;GjToken *t;} Parser;
static void ws(Parser *p){while(p->s[p->p]&&strchr(" \r\n\t",p->s[p->p]))p->p++;}
static int hx(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static int value(Parser *p,int depth){
    ws(p);if(depth>12||p->n==p->cap||!p->s[p->p])return -1;
    int i=p->n++;GjToken *t=&p->t[i];t->start=p->p;t->kind=p->s[p->p];
    if(t->kind=='{'||t->kind=='['){char close=t->kind=='{'?'}':']';p->p++;ws(p);
        if(p->s[p->p]!=close)for(;;){
            if(t->kind=='{'){ws(p);if(p->s[p->p]!='"'||value(p,depth+1)<0)return -1;ws(p);if(p->s[p->p++]!=':')return -1;}
            if(value(p,depth+1)<0)return -1;ws(p);if(p->s[p->p]!=',')break;p->p++;
        }
        if(p->s[p->p++]!=close)return -1;
    }else if(t->kind=='"'){
        p->p++;for(;;){unsigned char c=(unsigned char)p->s[p->p++];if(!c||c<32)return -1;if(c=='"')break;
            if(c=='\\'){char e=p->s[p->p++];if(!e)return -1;if(e=='u'){for(int j=0;j<4;j++)if(hx(p->s[p->p++])<0)return -1;}else if(!strchr("\"\\/bfnrt",e))return -1;}
        }
    }else{
        int start=p->p;while(p->s[p->p]&&!strchr(" ,]}\r\n\t",p->s[p->p]))p->p++;
        int len=p->p-start;const char *s=p->s+start;
        if(!((len==4&&!memcmp(s,"true",4))||(len==4&&!memcmp(s,"null",4))||(len==5&&!memcmp(s,"false",5)))){
            int k=0;if(s[k]=='-')k++;if(k>=len)return -1;
            if(s[k]=='0')k++;else{if(s[k]<'1'||s[k]>'9')return -1;while(k<len&&isdigit((unsigned char)s[k]))k++;}
            if(k<len&&s[k]=='.'){int at=++k;while(k<len&&isdigit((unsigned char)s[k]))k++;if(k==at)return -1;}
            if(k<len&&(s[k]=='e'||s[k]=='E')){k++;if(k<len&&(s[k]=='+'||s[k]=='-'))k++;int at=k;while(k<len&&isdigit((unsigned char)s[k]))k++;if(k==at)return -1;}
            if(k!=len)return -1;
        }
    }
    t->length=p->p-t->start;t->end=p->n;return i;
}
int gj_parse(const char *s,GjToken *t,int cap){Parser p={s,0,0,cap,t};if(value(&p,0)<0)return -1;ws(&p);return s[p.p]?-1:p.n;}
int gj_string(const char *s,const GjToken *t,int i,char *out,unsigned cap){
    if(i<0||t[i].kind!='"'||cap<1)return 0;unsigned n=0;
    int end=t[i].start+t[i].length-1;
    for(int p=t[i].start+1;p<end;p++){
        unsigned c=(unsigned char)s[p];if(c=='\\'){c=(unsigned char)s[++p];
            if(c=='u'){c=0;for(int j=0;j<4;j++)c=(c<<4)|(unsigned)hx(s[++p]);
                if(c>=0xd800&&c<=0xdbff){if(p+6>=end||s[p+1]!='\\'||s[p+2]!='u')return 0;p+=2;unsigned lo=0;for(int j=0;j<4;j++)lo=(lo<<4)|(unsigned)hx(s[++p]);if(lo<0xdc00||lo>0xdfff)return 0;c=0x10000+((c-0xd800)<<10)+lo-0xdc00;}
                else if(c>=0xdc00&&c<=0xdfff)return 0;
                if(!c||c<32)return 0;
                unsigned bytes=c<0x80?1:c<0x800?2:c<0x10000?3:4;if(n+bytes>=cap)return 0;
                if(bytes==1)out[n++]=(char)c;else{out[n++]=(char)((bytes==2?0xc0:bytes==3?0xe0:0xf0)|(c>>(6*(bytes-1))));for(unsigned j=bytes-1;j;j--)out[n++]=(char)(0x80|((c>>(6*(j-1)))&63));}continue;
            }
            if(c=='b'||c=='f'||c=='n'||c=='r'||c=='t')return 0;
        }
        if(n+1>=cap)return 0;out[n++]=(char)c;
    }out[n]=0;return 1;
}
int gj_field(const char *s,const GjToken *t,int obj,const char *key){
    if(obj<0||t[obj].kind!='{')return -1;
    for(int i=obj+1;i<t[obj].end;){char k[128];if(!gj_string(s,t,i,k,sizeof k))return -1;int v=i+1;if(!strcmp(k,key))return v;i=t[v].end;}return -1;
}
