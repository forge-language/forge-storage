/* Regression: fs_digest returns a thread-local buffer, so the receive primitive
 * must preserve the expected digest before it hashes newly downloaded bytes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern const char *fs_digest(const char *path);
extern int64_t fs_receive_file(const char *url, const char *digest, const char *path);
/* Upload JSON parsing is outside this focused download test. */
int64_t fw_parse(const char *text) { (void)text; return 0; }
int main(int argc, char **argv) {
 if(argc!=4)return 2;
 const char *expected=fs_digest(argv[2]);
 if(strlen(expected)!=64)return 3;
 if(fs_receive_file(argv[1],expected,argv[3])!=0) {
  fprintf(stderr,"Wrong bytes were accepted when the expected digest aliased fs_digest's buffer\n");return 4;
 }
 if(access(argv[3],F_OK)==0) {fprintf(stderr,"Rejected download created its destination\n");return 5;}
 char snapshot[65];snprintf(snapshot,sizeof(snapshot),"%s",fs_digest(argv[2]));
 if(fs_receive_file(argv[1],snapshot,argv[3])!=0)return 6;
 if(fs_receive_file(argv[1],"not-a-sha256",argv[3])!=0)return 7;
 return 0;
}
