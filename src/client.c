#define _GNU_SOURCE
#include <curl/curl.h>
#include <openssl/evp.h>
#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
extern int64_t fw_parse(const char *);
#define LIMIT (256ULL*1024*1024)
static _Thread_local char hash_buffer[65];
static _Thread_local char reply_buffer[8192];
struct sink { FILE *file; uint64_t size; };
static size_t file_write(char *data,size_t size,size_t count,void *ctx) {
 struct sink *s=ctx;size_t n=size*count;if(n>LIMIT-s->size)return 0;
 size_t done=fwrite(data,1,n,s->file);s->size+=done;return done;
}
static size_t reply_write(char *data,size_t size,size_t count,void *ctx) {
 size_t *used=ctx;size_t n=size*count;if(n>sizeof(reply_buffer)-1-*used)return 0;
 memcpy(reply_buffer+*used,data,n);*used+=n;reply_buffer[*used]=0;return n;
}
const char *fs_digest(const char *path) {
 hash_buffer[0]=0;int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return hash_buffer;
 struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||(uint64_t)st.st_size>LIMIT){close(fd);return hash_buffer;}
 EVP_MD_CTX *md=EVP_MD_CTX_new();unsigned char data[65536],hash[32];unsigned len=0;int ok=md&&EVP_DigestInit_ex(md,EVP_sha256(),NULL);
 while(ok){ssize_t n=read(fd,data,sizeof(data));if(n<0){ok=0;break;}if(!n)break;ok=EVP_DigestUpdate(md,data,(size_t)n);}
 if(ok) { ok=EVP_DigestFinal_ex(md,hash,&len); }
 EVP_MD_CTX_free(md);close(fd);
 if(ok&&len==32){for(unsigned i=0;i<32;i++)sprintf(hash_buffer+2*i,"%02x",hash[i]);}return hash_buffer;
}
static CURL *handle(const char *url) {
 CURL *c=curl_easy_init();if(!c)return NULL;
 curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
 curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,5L);curl_easy_setopt(c,CURLOPT_TIMEOUT,180L);
#if LIBCURL_VERSION_NUM >= 0x075500
 curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http,https");curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
 curl_easy_setopt(c,CURLOPT_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS,CURLPROTO_HTTPS);
#endif
 curl_easy_setopt(c,CURLOPT_USERAGENT,"forge-storage/0.1.0");return c;
}
int64_t fs_send_file(const char *url,const char *method,const char *token,const char *path) {
 FILE *f=fopen(path,"rb");if(!f)return 0;struct stat st;if(fstat(fileno(f),&st)||!S_ISREG(st.st_mode)||(uint64_t)st.st_size>LIMIT){fclose(f);return 0;}
 CURL *c=handle(url);if(!c){fclose(f);return 0;}size_t used=0;reply_buffer[0]=0;
 char auth[1024];if(strlen(token)>900){curl_easy_cleanup(c);fclose(f);return 0;}snprintf(auth,sizeof(auth),"Authorization: Bearer %s",token);
 struct curl_slist *headers=NULL;headers=curl_slist_append(headers,auth);headers=curl_slist_append(headers,"Content-Type: application/octet-stream");headers=curl_slist_append(headers,"Expect:");
 curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(c,CURLOPT_UPLOAD,1L);curl_easy_setopt(c,CURLOPT_CUSTOMREQUEST,method);
 curl_easy_setopt(c,CURLOPT_READDATA,f);curl_easy_setopt(c,CURLOPT_INFILESIZE_LARGE,(curl_off_t)st.st_size);
 curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,reply_write);curl_easy_setopt(c,CURLOPT_WRITEDATA,&used);
 CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
 curl_slist_free_all(headers);curl_easy_cleanup(c);fclose(f);
 return result==CURLE_OK&&(status==200||status==201)?fw_parse(reply_buffer):0;
}
static int fetch_file(const char *url,const char *path,int redirects) {
 FILE *f=fopen(path,"wb");if(!f)return 0;CURL *c=handle(url);if(!c){fclose(f);return 0;}
 struct sink s={f,0};curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,(long)redirects);curl_easy_setopt(c,CURLOPT_MAXREDIRS,3L);
 curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,file_write);curl_easy_setopt(c,CURLOPT_WRITEDATA,&s);
 CURLcode code=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(c);
 int ok=code==CURLE_OK&&status==200&&fflush(f)==0&&fsync(fileno(f))==0;if(fclose(f))ok=0;return ok;
}
int64_t fs_receive_file(const char *url,const char *digest,const char *path) {
 if(!url||!digest||strlen(digest)!=64||!path)return 0;
 /* Digest may alias the thread-local result of fs_digest; preserve it before hashing the download. */
 char expected[65];memcpy(expected,digest,sizeof(expected));
 char *tmp=NULL;if(asprintf(&tmp,"%s.part-XXXXXX",path)<0)return 0;int fd=mkstemp(tmp);if(fd<0){free(tmp);return 0;}close(fd);
 int ok=fetch_file(url,tmp,0)&&strcmp(fs_digest(tmp),expected)==0&&rename(tmp,path)==0;unlink(tmp);free(tmp);return ok;
}
int64_t fs_fetch_archive(const char *url,const char *base,const char *token) {
 if(strncmp(url,"https://codeload.github.com/",28)!=0)return 0;
 char tmp[]="/tmp/forge-artifact-XXXXXX";int fd=mkstemp(tmp);if(fd<0)return 0;close(fd);int64_t out=0;
 if(fetch_file(url,tmp,0)){char hash[65];snprintf(hash,sizeof(hash),"%s",fs_digest(tmp));char *target=NULL;if(strlen(hash)==64&&asprintf(&target,"%s/v1/objects/%s",base,hash)>=0){out=fs_send_file(target,"PUT",token,tmp);free(target);}}
 unlink(tmp);return out;
}
