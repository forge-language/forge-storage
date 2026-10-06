#define _GNU_SOURCE
#include <microhttpd.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
extern int64_t frmod_routes_authorize(int64_t);
extern int64_t frmod_routes_dispatch(int64_t);
extern int64_t fw_scope_begin(void);
extern int64_t fw_scope_end(void);
extern void fr_str_arena_reset(void);
#define LIMIT (256ULL*1024*1024)
struct request { struct MHD_Connection *conn; char *method,*path; char temp[4096]; int fd,answered; uint64_t size; EVP_MD_CTX *hash; };
static const char *root;
static _Thread_local char metadata[256];
const char *fs_method(int64_t h){return ((struct request*)(intptr_t)h)->method;}
const char *fs_path(int64_t h){return ((struct request*)(intptr_t)h)->path;}
const char *fs_header(int64_t h,const char *name){const char *s=MHD_lookup_connection_value(((struct request*)(intptr_t)h)->conn,MHD_HEADER_KIND,name);return s?s:"";}
int64_t fs_secret_equal(const char *a,const char *b){size_t n=strlen(a);return n==strlen(b)&&CRYPTO_memcmp(a,b,n)==0;}
static int digest_path(const char *hash,char *path,size_t size){if(strlen(hash)!=64)return 0;for(int i=0;i<64;i++)if(!((hash[i]>='a'&&hash[i]<='f')||(hash[i]>='0'&&hash[i]<='9')))return 0;return snprintf(path,size,"%s/%s",root,hash)<(int)size;}
int64_t fs_reply(int64_t h,int64_t status,const char *body){struct request *r=(void*)(intptr_t)h;struct MHD_Response *p=MHD_create_response_from_buffer(strlen(body),(void*)body,MHD_RESPMEM_MUST_COPY);if(!p)return MHD_NO;
 MHD_add_response_header(p,"Content-Type","application/json");MHD_add_response_header(p,"Cache-Control","no-store");MHD_add_response_header(p,"X-Content-Type-Options","nosniff");r->answered=1;int out=MHD_queue_response(r->conn,(unsigned)status,p);MHD_destroy_response(p);return out;}
const char *fs_metadata(const char *hash){char path[4096];struct stat st;metadata[0]=0;if(!digest_path(hash,path,sizeof(path))||lstat(path,&st)||!S_ISREG(st.st_mode))return metadata;snprintf(metadata,sizeof(metadata),"{\"sha256\":\"%s\",\"size\":%llu}",hash,(unsigned long long)st.st_size);return metadata;}
int64_t fs_commit(int64_t h,const char *expected){struct request *r=(void*)(intptr_t)h;unsigned char hash[32];unsigned size=0;char hex[65],target[4096];
 if(!r->hash||!EVP_DigestFinal_ex(r->hash,hash,&size)||size!=32) { return 500; }
 for(int i=0;i<32;i++)sprintf(hex+2*i,"%02x",hash[i]);
 if(!fs_secret_equal(hex,expected)) { return 422; }
 if(!digest_path(expected,target,sizeof(target))) { return 500; }
 if(fsync(r->fd)) { return 500; }
 int status=201;if(link(r->temp,target)){if(errno==EEXIST)status=200;else return 500;}
 int dir=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(dir<0||fsync(dir)){if(dir>=0)close(dir);return 500;}close(dir);return status;
}
int64_t fs_serve(int64_t h,const char *hash){struct request *r=(void*)(intptr_t)h;char path[4096];if(!digest_path(hash,path,sizeof(path)))return fs_reply(h,400,"{}");int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)){if(fd>=0)close(fd);return fs_reply(h,404,"{\"error\":\"not_found\"}");}
 char etag[70];snprintf(etag,sizeof(etag),"\"%s\"",hash);const char *match=fs_header(h,"If-None-Match");
 uint64_t start=0,end=st.st_size?(uint64_t)st.st_size-1:0;unsigned status=200;const char *range=fs_header(h,"Range");const char *ifrange=fs_header(h,"If-Range");
 if(strcmp(match,etag)==0){close(fd);struct MHD_Response *p=MHD_create_response_from_buffer(0,NULL,MHD_RESPMEM_PERSISTENT);MHD_add_response_header(p,"ETag",etag);MHD_add_response_header(p,"Cache-Control","public,max-age=31536000,immutable");r->answered=1;int out=MHD_queue_response(r->conn,304,p);MHD_destroy_response(p);return out;}
 if(*range&&(!*ifrange||strcmp(ifrange,etag)==0)){
  unsigned long long a=0,b=0;int used=0,ok=0;
  if(sscanf(range,"bytes=%llu-%llu%n",&a,&b,&used)==2&&range[used]==0){start=a;end=b;ok=1;}
  else if(sscanf(range,"bytes=%llu-%n",&a,&used)==1&&used>0&&range[used]==0){start=a;ok=1;}
  else if(sscanf(range,"bytes=-%llu%n",&a,&used)==1&&range[used]==0&&a>0){start=a>=(uint64_t)st.st_size?0:(uint64_t)st.st_size-a;ok=1;}
  if(end>=(uint64_t)st.st_size&&st.st_size)end=(uint64_t)st.st_size-1;
  if(!ok||!st.st_size||start>end||start>=(uint64_t)st.st_size){close(fd);struct MHD_Response *p=MHD_create_response_from_buffer(0,NULL,MHD_RESPMEM_PERSISTENT);char cr[80];snprintf(cr,sizeof(cr),"bytes */%llu",(unsigned long long)st.st_size);MHD_add_response_header(p,"Content-Range",cr);r->answered=1;int out=MHD_queue_response(r->conn,416,p);MHD_destroy_response(p);return out;}status=206;
 }
 uint64_t length=st.st_size?end-start+1:0;struct MHD_Response *p=MHD_create_response_from_fd_at_offset64(length,fd,start);if(!p){close(fd);return MHD_NO;}
 MHD_add_response_header(p,"Content-Type","application/octet-stream");MHD_add_response_header(p,"X-Content-Type-Options","nosniff");MHD_add_response_header(p,"Content-Disposition","attachment");MHD_add_response_header(p,"Accept-Ranges","bytes");MHD_add_response_header(p,"ETag",etag);MHD_add_response_header(p,"Cache-Control","public,max-age=31536000,immutable");
 if(status==206){char cr[100];snprintf(cr,sizeof(cr),"bytes %llu-%llu/%llu",(unsigned long long)start,(unsigned long long)end,(unsigned long long)st.st_size);MHD_add_response_header(p,"Content-Range",cr);}r->answered=1;int out=MHD_queue_response(r->conn,status,p);MHD_destroy_response(p);return out;
}
static void complete(void *cls,struct MHD_Connection *c,void **ctx,enum MHD_RequestTerminationCode code){(void)cls;(void)c;(void)code;struct request *r=*ctx;if(!r)return;if(r->fd>=0)close(r->fd);if(*r->temp)unlink(r->temp);EVP_MD_CTX_free(r->hash);free(r->method);free(r->path);free(r);*ctx=NULL;}
static enum MHD_Result access_request(void *cls,struct MHD_Connection *c,const char *url,const char *method,const char *version,const char *data,size_t *size,void **ctx){(void)cls;(void)version;struct request *r=*ctx;
 if(!r){r=calloc(1,sizeof(*r));if(!r)return MHD_NO;r->conn=c;r->fd=-1;r->method=strdup(method);r->path=strdup(url);if(!r->method||!r->path){free(r->method);free(r->path);free(r);return MHD_NO;}*ctx=r;
  fw_scope_begin();int allowed=frmod_routes_authorize((int64_t)(intptr_t)r);fw_scope_end();fr_str_arena_reset();if(!allowed)return r->answered?MHD_YES:MHD_NO;
  if(strcmp(method,"PUT")==0){const char *length=fs_header((int64_t)(intptr_t)r,"Content-Length");char *tail=NULL;unsigned long long n=strtoull(length,&tail,10);if(*length&&(!tail||*tail||n>LIMIT))return fs_reply((int64_t)(intptr_t)r,413,"{\"error\":\"too_large\"}");
   snprintf(r->temp,sizeof(r->temp),"%s/.upload-XXXXXX",root);r->fd=mkstemp(r->temp);r->hash=EVP_MD_CTX_new();if(r->fd<0||!r->hash||!EVP_DigestInit_ex(r->hash,EVP_sha256(),NULL))return fs_reply((int64_t)(intptr_t)r,500,"{}");}
  return MHD_YES;
 }
 if(r->answered){*size=0;return MHD_YES;}
 if(*size){size_t n=*size;*size=0;if(r->fd<0||n>LIMIT-r->size)return fs_reply((int64_t)(intptr_t)r,413,"{\"error\":\"too_large\"}");size_t offset=0;while(offset<n){ssize_t wrote=write(r->fd,data+offset,n-offset);if(wrote<0&&errno==EINTR)continue;if(wrote<=0)return MHD_NO;offset+=(size_t)wrote;}if(!EVP_DigestUpdate(r->hash,data,n))return MHD_NO;r->size+=n;return MHD_YES;}
 fw_scope_begin();int result=frmod_routes_dispatch((int64_t)(intptr_t)r);fw_scope_end();fr_str_arena_reset();return result;
}
static volatile sig_atomic_t stopping;
static void stop(int signal){(void)signal;stopping=1;}
int64_t fs_run(int64_t port){root=getenv("STORAGE_ROOT");if(!root)root="/data";if(port<1||port>65535||(mkdir(root,0700)&&errno!=EEXIST))return 1;
 struct stat root_stat;if(lstat(root,&root_stat)||!S_ISDIR(root_stat.st_mode)||S_ISLNK(root_stat.st_mode))return 1;
 signal(SIGTERM,stop);signal(SIGINT,stop);struct MHD_Daemon *d=MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD|MHD_USE_EPOLL,(uint16_t)port,NULL,NULL,access_request,NULL,MHD_OPTION_THREAD_POOL_SIZE,8U,MHD_OPTION_CONNECTION_LIMIT,128U,MHD_OPTION_CONNECTION_TIMEOUT,30U,MHD_OPTION_NOTIFY_COMPLETED,complete,NULL,MHD_OPTION_END);if(!d)return 1;while(!stopping)sleep(1);MHD_stop_daemon(d);return 0;
}
